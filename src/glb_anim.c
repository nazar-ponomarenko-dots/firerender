// ============================================================================
//  glb_anim.c — skeletal animation (skinning) from binary glTF 2.0 (.glb)
// ----------------------------------------------------------------------------
//  Two layers:
//    glb_anim_parse()  — CPU: types (accessors/buffers), node hierarchy, skins
//                        (joints + inverse bind matrices), primitives with weights
//                        (JOINTS_0/WEIGHTS_0), animation clips (channels+samplers).
//                        Geometry stays in THE BIND POSE.
//    glb_anim_make_node() — builds GPU buffers/textures, the skinned.vs +
//                        mesh.fs program, a node with update (ticks time) and render (skins).
//
//  Bone math (glTF 2.0 §4.8 / Appendix C):
//    jointMatrix(j) = globalTransformOfJointNode(j) * inverseBindMatrix(j)
//    skinnedVertex  = w0*J0*v + w1*J1*v + ...  (up to 4 bones per vertex)
//  The weight sum is normalized by dividing in the shader (or corrected here).
// ============================================================================
#include "glb_anim.h"

#include "events.h"
#include "json.h"
#include "linalg.h"
#include "physics.h"
#include "resources.h"
#include "shader.h"
#include "snowmask.h"
#include "frustum.h"
#include "stb_image.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#define C_BYTE 5120
#define C_UBYTE 5121
#define C_SHORT 5122
#define C_USHORT 5123
#define C_UINT 5125
#define C_FLOAT 5126

// fourcc of the GLB chunks: little-endian "JSON" and "BIN\0".
#define GLB_CHUNK_JSON 0x4E4F534A
#define GLB_CHUNK_BIN  0x004E4942

static int cs(int c){ switch(c){ case C_BYTE: case C_UBYTE: return 1;
                      case C_SHORT: case C_USHORT: return 2;
                      case C_UINT: case C_FLOAT: return 4; } return 0; }
static int tv(const char*t){ if(!t)return 1;
    if(!strcmp(t,"VEC2"))return 2;
    if(!strcmp(t,"VEC3"))return 3;
    if(!strcmp(t,"VEC4"))return 4;
    if(!strcmp(t,"MAT4"))return 16;
    return 1; }

typedef struct { int bufferView; long byteOffset; int comp, vec, norm; int stride; long count; } GA;
typedef struct { long byteOffset, byteLength; int byteStride; } GB;

static void m4id(float m[16]){ memset(m,0,16*sizeof(float)); m[0]=m[5]=m[10]=m[15]=1; }

// Which TEXCOORD set the material's baseColorTexture uses (0 by default).
// glTF allows up to two UV sets per primitive: characters' faces/eyes often
// live in TEXCOORD_1 — always reading only set zero = broken textures.
static int base_uv_set(const JsonNode*mt_a,const JsonNode*tx_a,int mat){
    if(mat<0||!mt_a||mt_a->type!=JSON_ARRAY)return 0;
    const JsonNode*m=json_array_get(mt_a,mat);
    const JsonNode*pbr=m?json_find(m,"pbrMetallicRoughness"):NULL;
    const JsonNode*tex=pbr?json_find(pbr,"baseColorTexture"):NULL;
    int ti=(int)json_num(tex,-1);
    if(ti<0||!tx_a||tx_a->type!=JSON_ARRAY)return 0;
    const JsonNode*t=json_array_get(tx_a,ti);
    return (int)json_num(t?json_find(t,"texCoord"):NULL,0);
}

// --- Shadows of skinned models ------------------------------------------------
// Depth pass (skinned_shadow.vs + mesh_shadow.fs): the same skinning as in the
// color pass — the model casts a shadow IN THE CURRENT FRAME'S POSE. One
// program shared by all anim nodes (module static, hot-reload via reshade hook).
static GLuint s_anim_shadow_prog=0;
static GLint s_as_model=-1,s_as_vp=-1,s_as_bones=-1,s_as_hasskin=-1,s_as_tex=-1,s_as_at=-1;
static void anim_shadow_init(void){
    s_as_model=glGetUniformLocation(s_anim_shadow_prog,"uModel");
    s_as_vp=glGetUniformLocation(s_anim_shadow_prog,"uViewProj");
    s_as_bones=glGetUniformLocation(s_anim_shadow_prog,"uBones");
    s_as_hasskin=glGetUniformLocation(s_anim_shadow_prog,"uHasSkin");
    s_as_tex=glGetUniformLocation(s_anim_shadow_prog,"uTex");
    s_as_at=glGetUniformLocation(s_anim_shadow_prog,"uAlphaTest");
}
static void anim_shadow_reshade(void){
    GLuint p=resources_program_replace(s_anim_shadow_prog,
                                       "shaders/skinned_shadow.vs",
                                       "shaders/mesh_shadow.fs");
    if(p)s_anim_shadow_prog=p;
    anim_shadow_init();
}
static void m4mul(float o[16],const float a[16],const float b[16]){
    float r[16];
    for(int c=0;c<4;c++)for(int rr=0;rr<4;rr++){
        float s=0;
        for(int k=0;k<4;k++) s+=a[k*4+rr]*b[c*4+k];
        r[c*4+rr]=s;
    }
    memcpy(o,r,sizeof r);
}
// quat (x,y,z,w) -> matrix
static void quat_mat(float m[16],const float q[4],const float s[3],const float t[3]){
    float x=q[0],y=q[1],z=q[2],w=q[3];
    m4id(m);
    m[0]=1-2*(y*y+z*z); m[1]=2*(x*y+z*w);   m[2]=2*(x*z-y*w);
    m[4]=2*(x*y-z*w);   m[5]=1-2*(x*x+z*z); m[6]=2*(y*z+x*w);
    m[8]=2*(x*z+y*w);   m[9]=2*(y*z-x*w);   m[10]=1-2*(x*x+y*y);
    for(int i=0;i<3;i++)for(int j=0;j<3;j++) m[j*4+i]*=s[i];
    m[12]=t[0]; m[13]=t[1]; m[14]=t[2];
}
static float rfv(const unsigned char*bin,long base,const GA*a,int elem,int comp){
    int sz=cs(a->comp); const unsigned char*p=bin+base+(long)elem*a->stride+(long)comp*sz;
    switch(a->comp){ case C_FLOAT:{float v;memcpy(&v,p,4);return v;}
        case C_UINT:{unsigned v;memcpy(&v,p,4);return (float)v;}
        case C_USHORT:{unsigned short v;memcpy(&v,p,2);return (float)v;}
        case C_SHORT:{short v;memcpy(&v,p,2);return (float)v;}
        case C_UBYTE:return (float)p[0]; case C_BYTE:return (float)((signed char)p[0]);} return 0;}
// Safe index read: unsigned SCALAR types only, range [0,maxIdx].
static long rfi(const unsigned char*bin,long base,const GA*a,int elem,long maxIdx){
    if(a->vec!=1) return -1;
    long idx=-1; const unsigned char*p=bin+base+(long)elem*a->stride;
    switch(a->comp){ case C_UINT:{unsigned v;memcpy(&v,p,4);idx=(long)v;break;}
        case C_USHORT:{unsigned short v;memcpy(&v,p,2);idx=(long)v;break;}
        case C_UBYTE:idx=(long)p[0];break; default:return -1;}
    if(idx<0) return -1;
    if(maxIdx>0&&idx>=maxIdx) return -1;
    return idx;}
static void rvv(const unsigned char*bin,long base,const GA*a,int elem,float*out){
    long st=a->stride; const unsigned char*p=bin+base+(long)elem*st;
    float f[16]={0};
    for(int c=0;c<a->vec;c++) switch(a->comp){
        case C_FLOAT:{float v;memcpy(&v,p+(long)c*4,4);f[c]=v;break;}
        case C_USHORT:{unsigned short v;memcpy(&v,p+(long)c*2,2);f[c]=a->norm?(float)v/65535.f:(float)v;break;}
        case C_UBYTE:{f[c]=a->norm?(float)p[c]/255.f:(float)p[c];break;}
        case C_BYTE:{f[c]=a->norm?(float)((signed char)p[c])/127.f:(float)((signed char)p[c]);break;}
        default:f[c]=0;} memcpy(out,f,(size_t)a->vec*sizeof(float)); }
static float clamp01(float x){ return x<0?0:(x>1?1:x); }
static float spec_from_rough(float rough,float metallic){
    float g=1-clamp01(rough); return 0.05f+g*(0.55f+0.45f*clamp01(metallic)); }

// An accessor must fit into its bufferView and the BIN chunk (protection
// against corrupt/malicious GLB: offsets/lengths come from JSON and MUST be
// validated against the real file size BEFORE reading). Read stride of rvv/rfv = a->stride.
static int acc_fits(const GA*a,const GB*bvs,int bv_n,long bin_len){
    if(!a||a->count<=0)return 0;
    if(a->bufferView<0||a->bufferView>=bv_n)return 0;
    const GB*bv=&bvs[a->bufferView];
    int sz=cs(a->comp)*a->vec; if(sz<=0)return 0;
    if(a->count>bin_len)return 0;
    // For interleaving: real size = (count-1)*stride + sz
    long stride = a->stride > 0 ? a->stride : sz;
    long need = (a->count - 1) * stride + sz;
    long base = bv->byteOffset + a->byteOffset;
    return base>=0&&bv->byteLength>=0&&need<=bv->byteLength&&base+need<=bin_len;
}
// bufferView (image/raw data) fully inside the BIN chunk
static int bv_fits(const GB*bvs,int bv_n,int i,long bin_len){
    if(i<0||i>=bv_n)return 0;
    return bvs[i].byteOffset>=0&&bvs[i].byteLength>=0&&
           bvs[i].byteOffset+bvs[i].byteLength<=bin_len;
}

// --- The parser itself ------------------------------------------------------------
AnimGlb *glb_anim_parse(const char *path)
{
    unsigned int filesz=0;
    unsigned char*file=resources_read_file(path,&filesz);
    if(!file){ SDL_Log("glb_anim: not found %s",path); return NULL; }
    if(filesz<20||memcmp(file,"glTF",4)){ SDL_Log("glb_anim: %s not a GLB",path); free(file); return NULL; }
    const unsigned char*json_data=NULL,*bin=NULL; unsigned int json_len=0,bin_len=0;
    unsigned int off=12;
    while(off+8<=filesz){ unsigned int clen,ctype; memcpy(&clen,file+off,4); memcpy(&ctype,file+off+4,4); off+=8;
        // clen from the file: off+clen can wrap around 32 bits and defeat the
        // check; bin_len would then become falsely huge and acc_fits would
        // validate against it, not the real buffer -> reads past the end.
        if(clen>filesz-off)break;
        if(ctype==GLB_CHUNK_JSON){json_data=file+off;json_len=clen;}
        else if(ctype==GLB_CHUNK_BIN){bin=file+off;bin_len=clen;}
        off+=clen; }
    if(!json_data||!bin){ SDL_Log("glb_anim: %s no chunks",path); free(file); return NULL; }
    const JsonNode*root=json_parse((const char*)json_data,(size_t)json_len);
    if(!root){ SDL_Log("glb_anim: %s JSON not recognized",path); free(file); return NULL; }

    const JsonNode*acc_a=json_find(root,"accessors"),*bv_a=json_find(root,"bufferViews");
    const JsonNode*nd_a=json_find(root,"nodes"),*ms_a=json_find(root,"meshes");
    const JsonNode*mt_a=json_find(root,"materials"),*tx_a=json_find(root,"textures");
    const JsonNode*im_a=json_find(root,"images"),*sm_a=json_find(root,"samplers");
    const JsonNode*sk_a=json_find(root,"skins"),*an_a=json_find(root,"animations");
    int acc_n=acc_a&&acc_a->type==JSON_ARRAY?json_array_len(acc_a):0;
    int bv_n=bv_a&&bv_a->type==JSON_ARRAY?json_array_len(bv_a):0;
    int node_n=nd_a&&nd_a->type==JSON_ARRAY?json_array_len(nd_a):0;
    if(node_n==0){ SDL_Log("glb_anim: %s no nodes",path); json_free(); free(file); return NULL; }

    GB* bvs=calloc((size_t)(bv_n?bv_n:1),sizeof* bvs);
    if(!bvs){ json_free(); free(file); return NULL; }
    for(int i=0;i<bv_n;i++){const JsonNode*b=json_array_get(bv_a,i);
        bvs[i].byteOffset=(long)json_num(json_find(b,"byteOffset"),0);
        bvs[i].byteLength=(long)json_num(json_find(b,"byteLength"),0);
        bvs[i].byteStride=(int)json_num(json_find(b,"byteStride"),0);}
    GA* accs=calloc((size_t)(acc_n?acc_n:1),sizeof* accs);
    if(!accs){ free(bvs); json_free(); free(file); return NULL; }
    for(int i=0;i<acc_n;i++){const JsonNode*a=json_array_get(acc_a,i);
        accs[i].bufferView=(int)json_num(json_find(a,"bufferView"),-1);
        accs[i].byteOffset=(long)json_num(json_find(a,"byteOffset"),0);
        accs[i].comp=(int)json_num(json_find(a,"componentType"),C_FLOAT);
        accs[i].vec=tv(json_str(json_find(a,"type"),"SCALAR"));
        accs[i].norm=json_bool(json_find(a,"normalized"),0);
        accs[i].count=(long)json_num(json_find(a,"count"),0);
        // Stride between elements: bufferView byteStride if given, else tight packing.
        int bv_i=accs[i].bufferView, elem_sz=cs(accs[i].comp)*accs[i].vec;
        accs[i].stride=(bv_i>=0&&bv_i<bv_n&&bvs[bv_i].byteStride>0)?bvs[bv_i].byteStride:elem_sz;}

    AnimGlb*out=calloc(1,sizeof* out);
    if(!out){ json_free(); free(bvs); free(accs); free(file); return NULL; }
    out->nodes=calloc((size_t)node_n,sizeof* out->nodes);
    if(!out->nodes){ SDL_Log("glb_anim: %s — out of memory",path);
        json_free(); free(bvs); free(accs); free(out); free(file); return NULL; }
    out->nnode=node_n;
    for(int i=0;i<node_n;i++) out->nodes[i].parent=-1;
    // parents and children
    for(int i=0;i<node_n;i++){const JsonNode*nd=json_array_get(nd_a,i); AnimNode*N=&out->nodes[i];
        const JsonNode*ch=json_find(nd,"children");
        snprintf(N->name,sizeof N->name,"%s",json_str(json_find(nd,"name"),""));
        if(ch&&ch->type==JSON_ARRAY){for(int k=0;k<json_array_len(ch);k++){int ci=(int)json_num(json_array_get(ch,k),-1);
            if(ci>=0&&ci<node_n&&N->nchildren<64){N->children[N->nchildren++]=ci; out->nodes[ci].parent=i;}}}
        N->mesh=(int)json_num(json_find(nd,"mesh"),-1);
        const JsonNode*mn=json_find(nd,"matrix");
        const JsonNode*tr=json_find(nd,"translation"),*qt=json_find(nd,"rotation"),*sc=json_find(nd,"scale");
        // base TRS components
        memcpy(N->btrans,(float[]){0,0,0},sizeof N->btrans);
        memcpy(N->bquat,(float[]){0,0,0,1},sizeof N->bquat);
        memcpy(N->bscl,(float[]){1,1,1},sizeof N->bscl);
        if(mn&&mn->type==JSON_ARRAY){for(int c=0;c<16;c++)N->trs[c]=(float)json_num(json_array_get(mn,c),0); N->has_matrix=1;}
        else {
            if(tr&&tr->type==JSON_ARRAY)for(int k=0;k<3&&k<json_array_len(tr);k++)N->btrans[k]=(float)json_num(json_array_get(tr,k),0);
            if(qt&&qt->type==JSON_ARRAY)for(int k=0;k<4&&k<json_array_len(qt);k++)N->bquat[k]=(float)json_num(json_array_get(qt,k),k==3?1:0);
            if(sc&&sc->type==JSON_ARRAY)for(int k=0;k<3&&k<json_array_len(sc);k++)N->bscl[k]=(float)json_num(json_array_get(sc,k),1);
        }
        if(!mn)N->has_matrix=0;
    }

    // skins
    int nskin=sk_a&&sk_a->type==JSON_ARRAY?json_array_len(sk_a):0;
    if(nskin>0){
        out->skins=calloc((size_t)nskin,sizeof* out->skins);
        out->skin_of_node=calloc((size_t)node_n,sizeof* out->skin_of_node);
        if(!out->skins||!out->skin_of_node){
            free(out->skins); free(out->skin_of_node);
            out->skins=NULL; out->skin_of_node=NULL; out->nskin=0; nskin=0;
        } else out->nskin=nskin;
        for(int i=0;i<node_n;i++)out->skin_of_node[i]=-1;
        for(int s=0;s<nskin;s++){const JsonNode*sk=json_array_get(sk_a,s);
            const JsonNode*jn=json_find(sk,"joints"); int jc=jn&&jn->type==JSON_ARRAY?json_array_len(jn):0;
            if(jc<0||jc>32*1024*1024) jc=0;      // cap on the number of joints
            AnimSkin*S=&out->skins[s]; S->joint_count=jc;
            S->joints=calloc((size_t)(jc?jc:1),sizeof* S->joints);
            S->inv_bind=calloc((size_t)(jc?jc:1)*16,sizeof* S->inv_bind);
            if(!S->joints||!S->inv_bind){ S->joint_count=0; continue; }
            for(int j=0;j<jc;j++)S->joints[j]=(int)json_num(json_array_get(jn,j),-1);
            int ibm=(int)json_num(json_find(sk,"inverseBindMatrices"),-1);
            if(ibm>=0&&ibm<acc_n&&accs[ibm].vec==16&&acc_fits(&accs[ibm],bvs,bv_n,(long)bin_len)){
                long base=accs[ibm].bufferView>=0&&accs[ibm].bufferView<bv_n?bvs[accs[ibm].bufferView].byteOffset+accs[ibm].byteOffset:0;
                int n=accs[ibm].count<(long)jc?accs[ibm].count:jc;
                for(int j=0;j<n;j++)for(int c=0;c<16;c++)S->inv_bind[j*16+c]=rfv(bin,base,&accs[ibm],j,c);
                for(int j=n;j<jc;j++)m4id(&S->inv_bind[j*16]);
            } else for(int j=0;j<jc;j++)m4id(&S->inv_bind[j*16]);
        }
    }

    // mesh primitives (in bind pose, local coords)
    out->skin_nodes=calloc((size_t)(node_n?node_n:1),sizeof* out->skin_nodes);
    int mesh_n=ms_a&&ms_a->type==JSON_ARRAY?json_array_len(ms_a):0;
    int*mesh_pstart=calloc((size_t)(mesh_n?mesh_n:1),sizeof(int));
    int*mesh_pcount=calloc((size_t)(mesh_n?mesh_n:1),sizeof(int));
    if(!out->skin_nodes||!mesh_pstart||!mesh_pcount){
        SDL_Log("glb_anim: %s — out of memory",path);
        json_free(); free(bvs); free(accs); glb_anim_free(out); free(file); return NULL; }
    for(int mi=0;mi<mesh_n;mi++){const JsonNode*mesh=json_array_get(ms_a,mi);
        const JsonNode*prims=json_find(mesh,"primitives");
        mesh_pstart[mi]=out->nprim;
        if(!prims||prims->type!=JSON_ARRAY){continue;}
        for(int p=0;p<json_array_len(prims);p++){const JsonNode*pr=json_array_get(prims,p);
            if((int)json_num(json_find(pr,"mode"),4)!=4)continue;
            const JsonNode*attrs=json_find(pr,"attributes");
            int ipos=(int)json_num(attrs?json_find(attrs,"POSITION"):NULL,-1);
            int inrm=(int)json_num(attrs?json_find(attrs,"NORMAL"):NULL,-1);
            // The UV set is the one referenced by the material's baseColorTexture
            // (glTF allows up to 2 UV sets: a character model's face may
            // live in TEXCOORD_1). Fallback — TEXCOORD_0.
            int mat_idx=(int)json_num(json_find(pr,"material"),-1);
            int uvset=base_uv_set(mt_a,tx_a,mat_idx);
            char uvkey[24]; snprintf(uvkey,sizeof uvkey,"TEXCOORD_%d",uvset);
            int iuv=(int)json_num(attrs?json_find(attrs,uvkey):NULL,-1);
            if(iuv<0) iuv=(int)json_num(attrs?json_find(attrs,"TEXCOORD_0"):NULL,-1);
            int ijo=(int)json_num(attrs?json_find(attrs,"JOINTS_0"):NULL,-1);
            int iwe=(int)json_num(attrs?json_find(attrs,"WEIGHTS_0"):NULL,-1);
            int iidx=(int)json_num(json_find(pr,"indices"),-1);
            if(ipos<0||ipos>=acc_n)continue;
            GA*ap=&accs[ipos];
            // OOB protection: the accessor must fit inside the file's BIN chunk.
            // Check vec right here: rvv copies vec floats into vp[3].
            if(!acc_fits(ap,bvs,bv_n,(long)bin_len)||ap->vec!=3){
                SDL_Log("glb_anim: %s — POSITION accessor out of buffer or not VEC3, primitive skipped",path);
                continue;
            }
            // The other attributes are read into stack receivers of fixed
            // size (n[3]/u[2]/ji[4]/we[4]) — if vec does not fit, do not read.
            GA*an=(inrm>=0&&inrm<acc_n&&accs[inrm].vec==3&&acc_fits(&accs[inrm],bvs,bv_n,(long)bin_len))?&accs[inrm]:NULL;
            GA*au=(iuv>=0&&iuv<acc_n&&accs[iuv].vec<=2&&acc_fits(&accs[iuv],bvs,bv_n,(long)bin_len))?&accs[iuv]:NULL;
            GA*aj=(ijo>=0&&ijo<acc_n&&accs[ijo].vec==4&&acc_fits(&accs[ijo],bvs,bv_n,(long)bin_len))?&accs[ijo]:NULL;
            GA*aw=(iwe>=0&&iwe<acc_n&&accs[iwe].vec==4&&acc_fits(&accs[iwe],bvs,bv_n,(long)bin_len))?&accs[iwe]:NULL;
            GA*ai=(iidx>=0&&iidx<acc_n&&acc_fits(&accs[iidx],bvs,bv_n,(long)bin_len))?&accs[iidx]:NULL;
            long nv=ap->count;
            // Reasonable cap on primitive vertex count (protection against count~1e9 from JSON)
            if(nv>16*1024*1024){
                SDL_Log("glb_anim: %s — POSITION count %ld too large",path,nv);
                continue;
            }
            AnimPrim *P = NULL;
            AnimPrim*o=realloc(out->prims,(size_t)(out->nprim+1)*sizeof* out->prims);
            if(!o) goto prim_fail;
            out->prims=o;
            P=&out->prims[out->nprim++]; memset(P,0,sizeof* P);
            {
                const JsonNode*mj=mat_idx>=0?json_array_get(mt_a,mat_idx):NULL;
                snprintf(P->matname,sizeof P->matname,"%s",
                         json_str(mj?json_find(mj,"name"):NULL,""));
            }
            P->verts=malloc((size_t)nv*3*sizeof(float));
            P->normals=malloc((size_t)nv*3*sizeof(float));
            P->uvs=calloc((size_t)nv*2,sizeof(float));
            if(aj&&aw){ P->bone_idx=malloc((size_t)nv*4*sizeof(int)); P->bone_w=malloc((size_t)nv*4*sizeof(float)); }
            if(!P->verts||!P->normals||!P->uvs||(aj&&aw&&(!P->bone_idx||!P->bone_w))) goto prim_fail;
            long pb=ap->bufferView>=0&&ap->bufferView<bv_n?bvs[ap->bufferView].byteOffset+ap->byteOffset:0;
            long nb=an&&an->bufferView>=0?bvs[an->bufferView].byteOffset+an->byteOffset:0;
            long ub=au&&au->bufferView>=0?bvs[au->bufferView].byteOffset+au->byteOffset:0;
            long jb=aj&&aj->bufferView>=0?bvs[aj->bufferView].byteOffset+aj->byteOffset:0;
            long wb=aw&&aw->bufferView>=0?bvs[aw->bufferView].byteOffset+aw->byteOffset:0;
            for(long v=0;v<nv;v++){
                float vp[3],n[3]={0,1,0},u[2]={0,0};
                rvv(bin,pb,ap,(int)v,vp);
                // NORMAL/UV/JOINTS/WEIGHTS are indexed by the same v, but acc_fits
                // checked each one against ITS OWN count. A corrupt GLB with count
                // < nv would otherwise read past the BIN chunk (OOB) — read an
                // attribute only within its own count, else use the default.
                if(an&&v<an->count) rvv(bin,nb,an,(int)v,n);
                if(au&&v<au->count) rvv(bin,ub,au,(int)v,u);
                P->verts[3*v]=vp[0]; P->verts[3*v+1]=vp[1]; P->verts[3*v+2]=vp[2];
                P->normals[3*v]=n[0]; P->normals[3*v+1]=n[1]; P->normals[3*v+2]=n[2];
                // NO V FLIP: stb_image puts the image's top row at t=0, and glTF
                // v=0 is also the top of the image (top-left origin).
                // Flipping mirrored the texture vertically: lips ended up on the forehead.
                P->uvs[2*v]=u[0]; P->uvs[2*v+1]=u[1];
                if(P->bone_idx){
                    float ji[4]={0,0,0,0},we[4]={0,0,0,0};
                    if(aj&&v<aj->count) rvv(bin,jb,aj,(int)v,ji);
                    if(aw&&v<aw->count) rvv(bin,wb,aw,(int)v,we);
                    for(int k=0;k<4;k++) P->bone_idx[4*v+k]=(int)ji[k];
                    for(int k=0;k<4;k++) P->bone_w[4*v+k]=we[k];
                }
            }
            if(ai){ long ib=ai->bufferView>=0&&ai->bufferView<bv_n?bvs[ai->bufferView].byteOffset+ai->byteOffset:0;
                P->indices=malloc((size_t)ai->count*sizeof(unsigned int)); if(!P->indices) goto prim_fail;
                for(long i=0;i<ai->count;i++){ long ii=rfi(bin,ib,ai,(int)i,nv);
                    if(ii>=0) P->indices[P->index_count++]=(unsigned int)ii; }
            }
            else {
                P->indices=malloc((size_t)nv*sizeof(unsigned int));
                if(!P->indices) goto prim_fail;
                for(long i=0;i<nv;i++) P->indices[i]=(unsigned)i;
                P->index_count=(int)nv;
            }
            P->vcount=(int)nv;
            goto prim_ok;
        prim_fail:
            // Allocation failure: free the half-built primitive and skip it.
            SDL_Log("glb_anim: %s — out of memory for primitive",path);
            if (P) {
                free(P->verts); free(P->normals); free(P->uvs);
                free(P->bone_idx); free(P->bone_w); free(P->indices);
                out->nprim--;
            }
            continue;
        prim_ok:;
            // material (2004-style pseudo-PBR)
            memcpy(P->tint,(float[]){1,1,1},sizeof P->tint); P->alpha=1; P->blend=MAT_BLEND_OPAQUE;
            P->cull=MAT_CULL_NONE; P->tile[0]=P->tile[1]=1; P->specular=0.2f;
            if(mat_idx>=0&&mt_a&&mt_a->type==JSON_ARRAY&&mat_idx<json_array_len(mt_a)){
                const JsonNode*mt=json_array_get(mt_a,mat_idx);
                const JsonNode*pbr=json_find(mt,"pbrMetallicRoughness");
                float rough=0.6f,metallic=0;
                if(pbr){const JsonNode*bcf=json_find(pbr,"baseColorFactor");
                    if(bcf&&bcf->type==JSON_ARRAY&&json_array_len(bcf)>=4){
                        P->tint[0]=(float)json_num(json_array_get(bcf,0),1);
                        P->tint[1]=(float)json_num(json_array_get(bcf,1),1);
                        P->tint[2]=(float)json_num(json_array_get(bcf,2),1);
                        P->alpha=(float)json_num(json_array_get(bcf,3),1);}
                    rough=(float)json_num(json_find(pbr,"roughnessFactor"),0.6f);
                    metallic=(float)json_num(json_find(pbr,"metallicFactor"),0);}
                P->specular=spec_from_rough(rough,metallic);
                const char*am=json_str(json_find(mt,"alphaMode"),"OPAQUE");
                if(!strcmp(am,"BLEND"))P->blend=MAT_BLEND_ALPHA;
                else if(!strcmp(am,"MASK"))P->blend=MAT_BLEND_ALPHATEST;
                // Render gltf props two-sided: otherwise single-sided interiors/
                // thin details disappear (you see "through" the glass and the inside
                // is not visible from outside). Ignore the file's doubleSided — always NONE.
                P->cull=MAT_CULL_NONE;
                const JsonNode*em=json_find(mt,"emissiveFactor");
                if(em&&em->type==JSON_ARRAY&&json_array_len(em)>=3){
                    P->tint[0]+=(float)json_num(json_array_get(em,0),0);
                    P->tint[1]+=(float)json_num(json_array_get(em,1),0);
                    P->tint[2]+=(float)json_num(json_array_get(em,2),0);}
                // albedo texture
                const JsonNode*bct=json_find(json_find(pbr,"baseColorTexture"),"index");
                int tidx=(int)json_num(bct,-1);
                const JsonNode*tex=(tidx>=0&&tx_a&&tx_a->type==JSON_ARRAY&&tidx<json_array_len(tx_a))?json_array_get(tx_a,tidx):NULL;
                int src=tex?(int)json_num(json_find(tex,"source"),-1):-1;
                int smp=tex?(int)json_num(json_find(tex,"sampler"),-1):-1;
                if(src>=0&&im_a&&im_a->type==JSON_ARRAY&&src<json_array_len(im_a)){
                    int iv=(int)json_num(json_find(json_array_get(im_a,src),"bufferView"),-1);
                    if(bv_fits(bvs,bv_n,iv,(long)bin_len)){int w,h,ch;
                        unsigned char*px=stbi_load_from_memory(bin+bvs[iv].byteOffset,(int)bvs[iv].byteLength,&w,&h,&ch,4);
                        if(px){P->img=px;P->iw=w;P->ih=h;P->wrap=MAT_WRAP_CLAMP;P->filter=MAT_FILTER_MIPMAP;
                            if(smp>=0&&sm_a&&sm_a->type==JSON_ARRAY&&smp<json_array_len(sm_a)){
                                const JsonNode*sa=json_array_get(sm_a,smp);
                                int wS=(int)json_num(json_find(sa,"wrapS"),33071);
                                int wT=(int)json_num(json_find(sa,"wrapT"),33071);
                                // REPEAT from the sampler pulls a 2nd tile when UV>1 -> torn
                                // artwork. For gltf props we use CLAMP (at the edge).
                                P->wrap=(wS==33648||wT==33648)?MAT_WRAP_MIRROR:MAT_WRAP_CLAMP;
                                int mag=(int)json_num(json_find(sa,"magFilter"),9729);
                                P->filter=(mag==9728)?MAT_FILTER_NEAREST:MAT_FILTER_MIPMAP;}
                        }}
                }
                if(P->blend==MAT_BLEND_ALPHA&&P->img&&P->alpha>=0.95f){
                    // Glass drawn with an opaque texture (alpha=255): without
                    // semi-transparent pixels BLEND does not blend and hides the
                    // interiors. If the texture alpha is fully 255 — it is glass.
                    int has_trans=0;
                    int npix=P->iw*P->ih;
                    const unsigned char*pa=P->img+3;
                    for(int k=0;k<npix;k++,pa+=4) if(*pa<250){has_trans=1;break;}
                    if(!has_trans) P->alpha=0.35f;
                }
            }
        }
        mesh_pcount[mi]=out->nprim-mesh_pstart[mi];
    }

    // animations
    int nclip=an_a&&an_a->type==JSON_ARRAY?json_array_len(an_a):0;
    if(nclip>0){
        out->clips=calloc((size_t)nclip,sizeof* out->clips);
        if(!out->clips){ out->nclip=0; nclip=0; } else out->nclip=nclip;
        for(int c=0;c<nclip;c++){const JsonNode*cl=json_array_get(an_a,c); AnimClip*C=&out->clips[c];
            const char*cname=json_str(json_find(cl,"name"),"");
            snprintf(C->name,sizeof C->name,"%s",cname[0]?cname:"unnamed");
            const JsonNode*ch_a=json_find(cl,"channels"); int nch=ch_a&&ch_a->type==JSON_ARRAY?json_array_len(ch_a):0;
            const JsonNode*sp_a=json_find(cl,"samplers");  int nsp=sp_a&&sp_a->type==JSON_ARRAY?json_array_len(sp_a):0;
            C->channels=calloc((size_t)(nch?nch:1),sizeof* C->channels);
            if(!C->channels) nch=0;
            C->nch=nch;
            C->samplers=calloc((size_t)(nsp?nsp:1),sizeof* C->samplers);
            if(!C->samplers) nsp=0;
            C->ns=nsp;
            float dur=0;
            for(int s=0;s<nsp;s++){const JsonNode*sp=json_array_get(sp_a,s); AnimSampler*Samp=&C->samplers[s];
                int inp=(int)json_num(json_find(sp,"input"),-1),outp=(int)json_num(json_find(sp,"output"),-1);
                const char*ip=json_str(json_find(sp,"interpolation"),"LINEAR");
                Samp->interp=!strcmp(ip,"STEP")?ANIM_STEP:(!strcmp(ip,"CUBICSPLINE")?ANIM_CUBIC:ANIM_LINEAR);
                if(inp>=0&&inp<acc_n&&outp>=0&&outp<acc_n&&
                   acc_fits(&accs[inp],bvs,bv_n,(long)bin_len)&&
                   acc_fits(&accs[outp],bvs,bv_n,(long)bin_len)){
                    GA*ti=&accs[inp],*vo=&accs[outp];
                    long nk=ti->count>16*1024*1024?0:ti->count;   // cap against OOM/OOB
                    // output-count must cover all keys, otherwise rfv reads
                    // past the accessor (OOB on a corrupt GLB): LINEAR/STEP — comp per
                    // key (need >= nk), CUBICSPLINE — 3*comp (need >= nk*3).
                    if(Samp->interp==ANIM_CUBIC){ if(vo->count<nk*3) nk=0; }
                    else if(vo->count<nk){ nk=0; }
                    Samp->comp=vo->vec; Samp->nkeys=nk;
                    // CUBICSPLINE stores 3*comp per key (in-tangent, value,
                    // out-tangent); LINEAR/STEP — comp per key.
                    int per=(Samp->interp==ANIM_CUBIC)?3*Samp->comp:Samp->comp;
                    Samp->times=malloc((size_t)(nk?nk:1)*sizeof(float));
                    Samp->values=malloc((size_t)(nk?(size_t)nk*per:1)*sizeof(float));
                    if(!Samp->times||!Samp->values){
                        free(Samp->times); free(Samp->values);
                        Samp->times=NULL; Samp->values=NULL; Samp->nkeys=0;
                        continue;
                    }
                    long tb=ti->bufferView>=0?bvs[ti->bufferView].byteOffset+ti->byteOffset:0;
                    long vb=vo->bufferView>=0?bvs[vo->bufferView].byteOffset+vo->byteOffset:0;
                    for(int k=0;k<nk;k++)Samp->times[k]=rfv(bin,tb,ti,k,0);
                    if(Samp->interp==ANIM_CUBIC){
                        // CUBICSPLINE: triple outputs (inTang, value, outTang)
                        for(int k=0;k<nk;k++)for(int q=0;q<Samp->comp;q++)for(int e=0;e<3;e++)
                            Samp->values[(k*3+e)*(size_t)Samp->comp+q]=
                                rfv(bin,vb,vo,(int)(k*3+e),q);
                    } else for(int k=0;k<nk;k++)for(int q=0;q<Samp->comp;q++)
                            Samp->values[k*(size_t)Samp->comp+q]=rfv(bin,vb,vo,k,q);
                    if(nk>0&&Samp->times[nk-1]>dur)dur=Samp->times[nk-1];
                }
            }
            C->duration=dur;
            for(int h=0;h<nch;h++){const JsonNode*ch=json_array_get(ch_a,h); AnimChannel*Ch=&C->channels[h];
                int sp_i=(int)json_num(json_find(ch,"sampler"),-1);
                const JsonNode*tg=json_find(ch,"target");
                Ch->node=(int)json_num(tg?json_find(tg,"node"):NULL,-1);
                const char*ph=json_str(tg?json_find(tg,"path"):NULL,"");
                Ch->path=!strcmp(ph,"rotation")?1:(!strcmp(ph,"scale")?2:0);
                Ch->sampler=sp_i;
            }
        }
    }

    // skin nodes (node with a skin)
    out->nskin_nodes=0;
    for(int i=0;i<node_n;i++){int sk=(int)json_num(json_find(json_array_get(nd_a,i),"skin"),-1);
        if(sk>=0&&sk<out->nskin){out->skin_of_node[i]=sk; out->skin_nodes[out->nskin_nodes++]=i;}}

    // instances: each node with a mesh → its own draw primitives (instancing allowed)
    int ninst=0;
    for(int i=0;i<node_n;i++){int m=out->nodes[i].mesh;
        if(m>=0&&m<mesh_n)ninst+=mesh_pcount[m];}
    out->instances=calloc((size_t)(ninst?ninst:1),sizeof* out->instances);
    if(!out->instances) out->ninst=0; else {
    out->ninst=ninst;
    for(int i=0,k=0;i<node_n;i++){int m=out->nodes[i].mesh;
        if(m<0||m>=mesh_n)continue;
        for(int p=0;p<mesh_pcount[m];p++)
            out->instances[k++]=(AnimInstance){i,mesh_pstart[m]+p,out->skin_of_node[i]};}
    }
    free(mesh_pstart); free(mesh_pcount);

    free(bvs); free(accs);
    json_free(); free(file);
    if(out->nprim==0&&out->nnode>0){ out->err[0]=0; /* no primitives — fine, empty */ }
    return out;
}

void glb_anim_free(AnimGlb*a){ if(!a)return;
    for(int i=0;i<a->nprim;i++){AnimPrim*P=&a->prims[i];
        free(P->verts);free(P->normals);free(P->uvs);free(P->indices);
        free(P->bone_idx);free(P->bone_w);stbi_image_free(P->img);} free(a->prims);
    free(a->nodes);
    for(int s=0;s<a->nskin;s++){free(a->skins[s].joints);free(a->skins[s].inv_bind);} free(a->skins);
    free(a->skin_of_node);
    for(int c=0;c<a->nclip;c++){free(a->clips[c].channels); 
        for(int s=0;s<a->clips[c].ns;s++){free(a->clips[c].samplers[s].times);free(a->clips[c].samplers[s].values);}
        free(a->clips[c].samplers);} free(a->clips);
    free(a->skin_nodes); free(a->instances); free(a); }

// --- Fast peek: is there skin/animation ---------------------------------------
int glb_anim_has_anim(const char*path){
    unsigned int sz=0; unsigned char*f=resources_read_file(path,&sz);
    if(!f||sz<20)return 0;
    off_t off=12; int skin=0,anim=0;
    while(off+8<=sz){
        unsigned clen,ct; memcpy(&clen,f+off,4); memcpy(&ct,f+off+4,4); off+=8;
        if(off+clen>sz) break;
        if(ct==GLB_CHUNK_JSON){
            const JsonNode*r=json_parse((const char*)f+off,clen);
            if(r){
                const JsonNode*sk=json_find(r,"skins");
                if(sk&&sk->type==JSON_ARRAY&&json_array_len(sk)>0) skin=1;
                const JsonNode*an=json_find(r,"animations");
                if(an&&an->type==JSON_ARRAY&&json_array_len(an)>0) anim=1;
            }
            json_free(); break;
        }
        off+=clen;
    }
    free(f); return skin&&anim;
}

// --- Node (GPU) ----------------------------------------------------------------
#define MAX_BONES 64
static void anim_node_exit(const Event*,void*);   // forward (see below)
typedef struct{
    GLuint vao,vbo,ebo; int index_count; GLenum index_type;
    GLuint snow_vbo;     // per-vertex "open to the sky above" (snow), 0 if none
    GLuint tex; Material mat; MaterialLocs ml; float specular;
    int skin;            // skin index (-1 — no skinning)
    int has_skin;        // render with bones
    int node;            // node carrying this mesh (for non-skinned: its node_world)
    int tex_owned;       // 1 — glGenTextures (from img), 0 — resource cache (notfound)
    GLuint joints_bo, weights_bo;
    int *joint_map;      // which bones of THIS SKIN are actually used (distinct,
    int joint_n;         // positions in its joints[]); vertex local indices — 0..joint_n-1
    float *bone;         // jointMatrix of these bones for uBones (joint_n*16)
    char mtl_name[64];   // material name (GROUND-plane filter in the ragdoll)
    int rag_skip;        // 1 — do not draw when ragdoll is enabled
} AnimDrawItem;
typedef struct{
    const RendererContext*rc; AnimDrawItem*items; int count;
    GLuint prog; SceneUniforms su;
    GLint u_model,u_tex,u_specular,u_bones,u_hasskin,u_uvtile;
    float scale; float anim_time; float anim_speed;
    int paused; int loop;       // playback control
    AnimGlb*data;           // grows — model data (not uploaded to the GPU each frame)
    float*node_world;       // global node matrices (recomputed every frame)
    float*skin;             // jointMatrix of all bones of the skin being processed
    int skin_cap;           // capacity of skin[] (max joint_count across skins)
    float*wtrs;             // working node TRS: [i*10] = trans3+quat4+scl3 (animated)
    int cur_clip;           // number of the active clip
    float radius;           // bounding sphere of geometry in world scale
    char path[256];         // GLB path (for the anim/list tool)
    int enabled;            // 0 — hidden and time does not run, 1 — visible/playing
    float dest[3]; float move_speed; int moving;   // smooth move toward dest
    void *rag;              // ragdoll (AnimRag*) — toggled by phys_ragdoll
} AnimParams;

// Global registry of anim nodes (numbering = load order). A node's index is
// fixed at registration and never changes — the `anim <index>` tool addresses
// nodes by it (nodes removed via anim remove are no longer handed out).
typedef struct AnimRegEntry { SceneNode *node; struct AnimRegEntry *next; } AnimRegEntry;
static AnimRegEntry *s_anim_reg = NULL;
static void anim_reg_add(SceneNode *n){
    AnimRegEntry *e = malloc(sizeof *e); if (!e) return;
    e->node = n; e->next = s_anim_reg; s_anim_reg = e;
}
static void anim_reg_remove(SceneNode *n){
    for (AnimRegEntry **p = &s_anim_reg; *p; p = &(*p)->next) {
        if ((*p)->node == n) { AnimRegEntry *d = *p; *p = d->next; free(d); return; }
    }
}
static SceneNode *anim_reg_find(int idx){
    if (idx < 0) return NULL;
    for (AnimRegEntry *e = s_anim_reg; e; e = e->next)
        if (idx-- == 0) return e->node;
    return NULL;
}

// --- Ragdoll (Verlet): particles = skeleton bones ---------------------------------
// Toggled by phys_ragdoll: each bone gets a particle at its world point,
// links follow the hierarchy (+ links through the node for joint stiffness).
// Animation is FULLY replaced by physics: every frame ragdoll_apply builds
// node_world from the particles (bone direction = first child's particle minus
// the bone's particle; the frame is built from TWO vectors — "forward" and
// "side" (side particle), with swing+twist: torsion propagates along the
// chain smoothly, the skin does not twist into a candy wrapper), then step 3
// — jointMatrix = node_world*inv_bind.
// Joint limits: the angle at a joint never goes beyond rest ± limit.
typedef struct {
    PhysWorld *w;              // particles in WORLD space
    SceneNode *owner;          // owning node (instance matrix)
    int nn;                    // number of model nodes
    int *order;                // topological order (parents before children)
    int *part;                 // node -> particle
    int *cpart;                // node -> first child's particle (-1 leaf)
    int *spart;                // node -> SIDE particle (-1 none; pins the roll)
    int origin_part;           // "_origin" particle (model anchor point)
    float (*F0)[3];            // rest "forward" of the bone (world, toward the child)
    float (*U0)[3];            // rest "side" of the bone (world, local X axis)
    float *rw0;                // rest world rotation of the node (16/node)
    float *Mw;                 // working world bone matrices (16/node, frame)
    float *rsw;                // rest world SCALE of the node (each has its own!)
    float *tha0;               // rest angle at the joint (parent->node->child)
    int hang;                  // 1 — origin pinned (hangs), 0 — falls as a whole
    int dbg_done;
    int dbg_calls;
} AnimRag;

static void rag_free(AnimRag *r)
{
    if (!r) return;
    free(r->w); free(r->part); free(r->cpart); free(r->order);
    free(r->spart); free(r->F0); free(r->U0); free(r->rw0); free(r->Mw);
    free(r->rsw); free(r->tha0);
    free(r);
}

// Shortest 3x3 rotation mapping unit vector a onto b (Rodrigues)
static void m3_a_to_b(const float *a, const float *b, float *r)
{
    float ax = a[1]*b[2] - a[2]*b[1];   // cross(a, b)
    float ay = a[2]*b[0] - a[0]*b[2];
    float az = a[0]*b[1] - a[1]*b[0];
    float c = a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
    float s = sqrtf(ax*ax + ay*ay + az*az);
    float K[9] = { 0, -az, ay,  az, 0, -ax,  -ay, ax, 0 };
    if (s < 1e-6f) {
        if (c < 0.0f) {   // opposite — 180° flip around a perpendicular
            float px = 0.0f, py = 1.0f, pz = 0.0f;
            if (fabsf(a[1]) > 0.9f) { py = 0.0f; pz = 1.0f; }
            float vx = a[1]*pz - a[2]*py;
            float vy = a[2]*px - a[0]*pz;
            float vz = a[0]*py - a[1]*px;
            float l = sqrtf(vx*vx + vy*vy + vz*vz);
            if (l > 1e-6f) { vx /= l; vy /= l; vz /= l; }
            r[0]=2*vx*vx-1; r[1]=2*vx*vy;   r[2]=2*vx*vz;
            r[3]=2*vy*vx;   r[4]=2*vy*vy-1; r[5]=2*vy*vz;
            r[6]=2*vz*vx;   r[7]=2*vz*vy;   r[8]=2*vz*vz-1;
        } else {
            r[0]=1; r[1]=0; r[2]=0; r[3]=0; r[4]=1; r[5]=0; r[6]=0; r[7]=0; r[8]=1;
        }
        return;
    }
    float k = (1.0f - c) / (s * s);
    float k2[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            k2[i*3+j] = K[i*3+0]*K[0*3+j] + K[i*3+1]*K[1*3+j] + K[i*3+2]*K[2*3+j];
    float I[9] = { 1,0,0, 0,1,0, 0,0,1 };
    for (int i = 0; i < 9; i++)
        r[i] = I[i] + K[i] + k2[i] * k;
}

// Rotation part of the matrix with normalized columns (removes scale)
static void m4_rot_norm(const float *m, float *o)
{
    float cx[3] = { m[0], m[1], m[2] };
    float cy[3] = { m[4], m[5], m[6] };
    float cz[3] = { m[8], m[9], m[10] };
    float lx = sqrtf(cx[0]*cx[0] + cx[1]*cx[1] + cx[2]*cx[2]);
    float ly = sqrtf(cy[0]*cy[0] + cy[1]*cy[1] + cy[2]*cy[2]);
    float lz = sqrtf(cz[0]*cz[0] + cz[1]*cz[1] + cz[2]*cz[2]);
    if (lx < 1e-8f) lx = 1.0f;
    if (ly < 1e-8f) ly = 1.0f;
    if (lz < 1e-8f) lz = 1.0f;
    memset(o, 0, 16 * sizeof(float));
    o[0] = cx[0]/lx; o[1] = cx[1]/lx; o[2]  = cx[2]/lx;
    o[4] = cy[0]/ly; o[5] = cy[1]/ly; o[6]  = cy[2]/ly;
    o[8] = cz[0]/lz; o[9] = cz[1]/lz; o[10] = cz[2]/lz;
    o[15] = 1.0f;
}

// Enable ragdoll on an animated model. hang=1 — origin pinned (hangs).
int glb_anim_ragdoll_enable(SceneNode *n, int hang)
{
    AnimParams *p = n ? (AnimParams *)n->params : NULL;
    if (!p || !p->data || !p->node_world || p->rag) return -1;
    AnimGlb *a = p->data;
    int nn = a->nnode;
    int side_count = 0, link_count = 2 * nn;
    for (int i = 0; i < nn; i++) {
        const AnimNode *N = &a->nodes[i];
        if (N->nchildren > 0 && N->children[0] >= 0 && N->children[0] < nn) {
            side_count++;
            link_count += a->nodes[i].parent >= 0 ? 4 : 3;
        }
    }
    if (nn + side_count + 1 > PHYS_MAX_PARTS || link_count > PHYS_MAX_LINKS) {
        SDL_Log("ragdoll: skeleton too large (%d/%d parts, %d/%d links)",
                nn + side_count + 1, PHYS_MAX_PARTS,
                link_count, PHYS_MAX_LINKS);
        return -1;
    }

    AnimRag *r = calloc(1, sizeof *r);
    if (!r) return -1;
    r->w = malloc(sizeof(PhysWorld));
    if (!r->w) { rag_free(r); return -1; }
    physics_reset(r->w);
    // A long bone chain needs many relaxation iterations: at 6 the body
    // "melts" — leaves drift meters away from the root (links cannot converge in time)
    r->w->iterations = 32;
    r->owner = n;
    r->nn = nn;
    r->hang = hang ? 1 : 0;
    r->part   = malloc((size_t)nn * sizeof(int));
    r->cpart  = malloc((size_t)nn * sizeof(int));
    r->spart  = malloc((size_t)nn * sizeof(int));
    r->order  = malloc((size_t)nn * sizeof(int));
    r->F0     = calloc((size_t)nn, sizeof(*r->F0));
    r->U0     = calloc((size_t)nn, sizeof(*r->U0));
    r->rw0    = calloc((size_t)nn, 16 * sizeof(float));
    r->Mw     = calloc((size_t)nn, 16 * sizeof(float));
    r->rsw    = calloc((size_t)nn, sizeof(float));
    r->tha0   = calloc((size_t)nn, sizeof(float));
    if (!r->w || !r->part || !r->cpart || !r->spart || !r->order || !r->F0 ||
        !r->U0 || !r->rw0 || !r->Mw || !r->rsw || !r->tha0) {
        rag_free(r);
        return -1;
    }

    // Rest world node matrices (last frame's pose + instance)
    static float wi[PHYS_MAX_PARTS][16];
    for (int i = 0; i < nn; i++)
        m4mul(wi[i], n->world, &p->node_world[i * 16]);

    // Topological order: roots first (children after parents)
    int on = 0;
    for (int i = 0; i < nn; i++)
        if (a->nodes[i].parent < 0) r->order[on++] = i;
    for (int pass = 0; pass < 64 && on < nn; pass++) {
        for (int i = 0; i < nn; i++) {
            int par = a->nodes[i].parent;
            if (par < 0) continue;
            int placed = 0, me = 0;
            for (int k2 = 0; k2 < on; k2++) {
                if (r->order[k2] == par) placed = 1;
                if (r->order[k2] == i) me = 1;
            }
            if (placed && !me) r->order[on++] = i;
        }
    }
    if (on < nn) {   // cycles in the hierarchy — ragdoll impossible on such a model
        rag_free(r);
        return -1;
    }

    // Particles at the current world bone points (pass 1), then particles of
    // the first children (pass 2 — part[] is fully filled by then, otherwise we
    // would read malloc garbage for children with an index above the current one)
    for (int i = 0; i < nn; i++) {
        r->part[i] = physics_add_part(r->w, wi[i] + 12, 1.0f, 0.09f);
        if (r->part[i] < 0) { rag_free(r); return -1; }
    }
    for (int i = 0; i < nn; i++) {
        const AnimNode *N = &a->nodes[i];
        r->cpart[i] = (N->nchildren > 0 && N->children[0] >= 0 &&
                       N->children[0] < nn)
                          ? r->part[N->children[0]] : -1;
    }
    float op[3] = { n->world[12], n->world[13], n->world[14] };
    r->origin_part = physics_add_part(r->w, op, 1.0f, 0.09f);
    if (r->origin_part < 0) { rag_free(r); return -1; }
    if (r->hang) physics_pin(r->w, r->origin_part, 1);

    // --- Rest frame of each bone: scale, rotation, "forward", "side" ------
    // "Side" (side particle) pins the bone's ROLL: without it the swing
    // reconstruction twists the skin into a candy (each bone spins on its own).
    for (int k = 0; k < nn; k++) {
        int i = r->order[k];
        int par = a->nodes[i].parent;
        // Rest world node SCALE: skeleton nodes often carry their own
        // scale (0.01 etc.) — without it the reconstruction crushes the pieces
        r->rsw[i] = sqrtf(wi[i][0]*wi[i][0] + wi[i][1]*wi[i][1]
                        + wi[i][2]*wi[i][2]);
        if (r->rsw[i] < 1e-8f) r->rsw[i] = 1.0f;
        m4_rot_norm(wi[i], &r->rw0[i * 16]);
        // link to the parent (+ through the node to the grandparent — joint stiffness)
        if (physics_add_link(r->w, r->part[i],
                             par >= 0 ? r->part[par] : r->origin_part) < 0) {
            rag_free(r); return -1;
        }
        int gp = par >= 0 ? a->nodes[par].parent : -1;
        if (physics_add_link(r->w, r->part[i],
                             gp >= 0 ? r->part[gp] : r->origin_part) < 0) {
            rag_free(r); return -1;
        }
        // rest joint angle: (parent->node) versus (node->child)
        const AnimNode *N = &a->nodes[i];
        if (r->cpart[i] >= 0) {
            int ch = N->children[0];
            float d1[3], d2[3];
            if (par >= 0) {
                d1[0] = wi[i][12] - wi[par][12];
                d1[1] = wi[i][13] - wi[par][13];
                d1[2] = wi[i][14] - wi[par][14];
            } else {
                d1[0] = op[0] - wi[i][12]; d1[1] = op[1] - wi[i][13];
                d1[2] = op[2] - wi[i][14];
            }
            d2[0] = wi[ch][12] - wi[i][12];
            d2[1] = wi[ch][13] - wi[i][13];
            d2[2] = wi[ch][14] - wi[i][14];
            float l1 = sqrtf(d1[0]*d1[0]+d1[1]*d1[1]+d1[2]*d1[2]);
            float l2 = sqrtf(d2[0]*d2[0]+d2[1]*d2[1]+d2[2]*d2[2]);
            if (l1 > 1e-5f && l2 > 1e-5f) {
                float c = (d1[0]*d2[0] + d1[1]*d2[1] + d1[2]*d2[2]) / (l1*l2);
                r->tha0[i] = acosf(c < -1.0f ? -1.0f : (c > 1.0f ? 1.0f : c));
            }
            // "forward" — toward the child
            float l = l2;
            r->F0[i][0] = d2[0] / l; r->F0[i][1] = d2[1] / l;
            r->F0[i][2] = d2[2] / l;
            // "side" — the bone's local X axis (stable per the rest frame)
            r->U0[i][0] = (&r->rw0[i * 16])[0];
            r->U0[i][1] = (&r->rw0[i * 16])[1];
            r->U0[i][2] = (&r->rw0[i * 16])[2];
            // Side particle: the (node, child, side) triangle rigidly pins the
            // segment's roll, and the link to the parent's side particle drags torsion
            // along the whole chain — the skin does not twist.
            float fl = l2;
            float sl = 0.3f * fl; if (sl < 0.04f) sl = 0.04f;
            float sp[3] = { wi[i][12] + r->U0[i][0] * sl,
                            wi[i][13] + r->U0[i][1] * sl,
                            wi[i][14] + r->U0[i][2] * sl };
            r->spart[i] = physics_add_part(r->w, sp, 1.0f, 0.03f);
            if (r->spart[i] < 0 ||
                physics_add_link(r->w, r->spart[i], r->part[i]) < 0 ||
                physics_add_link(r->w, r->spart[i], r->cpart[i]) < 0) {
                rag_free(r); return -1;
            }
            int psp = (par >= 0) ? r->spart[par] : -1;
            if (psp >= 0) {
                if (physics_add_link(r->w, r->spart[i], psp) < 0 ||
                    physics_add_link(r->w, r->spart[i], r->part[par]) < 0) {
                    rag_free(r); return -1;
                }
            } else if (physics_add_link(r->w, r->spart[i], r->origin_part) < 0) {
                rag_free(r); return -1;
            }
        } else {
            r->spart[i] = -1;   // leaf — frame inherited from the parent
        }
    }
    p->rag = r;
    // Hide the model's helper planes (GROUND material etc.) —
    // in ragdoll they get dragged by bones and unfurl into giant sails
    for (int i = 0; i < p->count; i++) {
        char low[64];
        snprintf(low, sizeof low, "%s", p->items[i].mtl_name);
        for (char *c = low; *c; c++)
            if (*c >= 'A' && *c <= 'Z') *c += 32;
        p->items[i].rag_skip = strstr(low, "ground") != NULL;
        SDL_Log("rag item[%d]: '%s' skin=%d skip=%d", i,
                p->items[i].mtl_name, p->items[i].has_skin,
                p->items[i].rag_skip);
    }
    SDL_Log("ragdoll: %d bones, %s", nn, r->hang ? "hangs" : "falls");
    return 0;
}

// Override node_world with matrices from the particles (called from build_bones
// after step 2, BEFORE step 3 jointMatrix — skinning takes the ragdoll pose).
static void ragdoll_apply(AnimParams *p)
{
    AnimRag *r = p->rag;
    AnimGlb *a = p->data;
    PhysWorld *w = r->w;
    float inv_inst[16];
    mat4_invert(inv_inst, r->owner->world);

    for (int k = 0; k < r->nn; k++) {
        int i = r->order[k];
        int par = a->nodes[i].parent;
        // Bone frame: SWING + TWIST.
        // 1) Swing: rest "forward" (F0) is rotated by the shortest arc onto the
        //    current direction to the child — the bone's main orientation.
        // 2) Twist: the side particle sets the roll around the bone axis; the
        //    rotated rest "side" (U1) is turned onto it — torsion travels along
        //    the chain through side-particle links smoothly, the skin does not curl up.
        float Mrot[16];
        if (r->cpart[i] >= 0 && r->spart[i] >= 0) {
            const float *pi = w->part[r->part[i]].pos;
            const float *pc = w->part[r->cpart[i]].pos;
            float F[3] = { pc[0]-pi[0], pc[1]-pi[1], pc[2]-pi[2] };
            float fl = sqrtf(F[0]*F[0] + F[1]*F[1] + F[2]*F[2]);
            if (fl > 1e-5f) { F[0] /= fl; F[1] /= fl; F[2] /= fl; }
            float R1[16];
            float r9[9];
            m3_a_to_b(r->F0[i], F, r9);
            memset(R1, 0, sizeof R1); R1[15] = 1.0f;
            R1[0] = r9[0]; R1[1] = r9[1]; R1[2]  = r9[2];
            R1[4] = r9[3]; R1[5] = r9[4]; R1[6]  = r9[5];
            R1[8] = r9[6]; R1[9] = r9[7]; R1[10] = r9[8];
            // Twist: angle between U1 (=R1*U0) and the current "side" around F
            float U1[3], Uc[3];
            const float *ps = w->part[r->spart[i]].pos;
            for (int q = 0; q < 3; q++) {
                U1[q] = R1[q*4+0]*r->U0[i][0] + R1[q*4+1]*r->U0[i][1]
                      + R1[q*4+2]*r->U0[i][2];
                Uc[q] = ps[q] - pi[q];
            }
            float u1f = U1[0]*F[0] + U1[1]*F[1] + U1[2]*F[2];
            float ucf = Uc[0]*F[0] + Uc[1]*F[1] + Uc[2]*F[2];
            for (int q = 0; q < 3; q++) {
                U1[q] -= F[q] * u1f;
                Uc[q] -= F[q] * ucf;
            }
            float l1 = sqrtf(U1[0]*U1[0] + U1[1]*U1[1] + U1[2]*U1[2]);
            float lc = sqrtf(Uc[0]*Uc[0] + Uc[1]*Uc[1] + Uc[2]*Uc[2]);
            if (l1 > 1e-5f && lc > 1e-5f) {
                for (int q = 0; q < 3; q++) { U1[q] /= l1; Uc[q] /= lc; }
                float cr[3] = { U1[1]*Uc[2] - U1[2]*Uc[1],
                                U1[2]*Uc[0] - U1[0]*Uc[2],
                                U1[0]*Uc[1] - U1[1]*Uc[0] };
                float sn = cr[0]*F[0] + cr[1]*F[1] + cr[2]*F[2];
                float cs = U1[0]*Uc[0] + U1[1]*Uc[1] + U1[2]*Uc[2];
                if (cs < -1.0f) cs = -1.0f; else if (cs > 1.0f) cs = 1.0f;
                float phi = atan2f(sn, cs);
                // R2 = rotation around F by phi (Rodrigues, column-major)
                float fx = F[0], fy = F[1], fz = F[2];
                float c = cosf(phi), s = sinf(phi), t = 1.0f - c;
                float R2[16];
                R2[0] = c + t*fx*fx;     R2[4] = t*fx*fy - s*fz;
                R2[8] = t*fx*fz + s*fy;
                R2[1] = t*fy*fx + s*fz;  R2[5] = c + t*fy*fy;
                R2[9] = t*fy*fz - s*fx;
                R2[2] = t*fz*fx - s*fy;  R2[6] = t*fz*fy + s*fx;
                R2[10] = c + t*fz*fz;
                R2[3] = R2[7] = R2[11] = 0.0f;
                R2[12] = R2[13] = R2[14] = 0.0f; R2[15] = 1.0f;
                float A[16];
                m4mul(A, R2, R1);
                m4mul(Mrot, A, &r->rw0[i * 16]);
            } else {
                float A[16];
                m4mul(A, R1, &r->rw0[i * 16]);
                memcpy(Mrot, A, sizeof A);
            }
        } else {
            // Leaf (hand/foot): the parent's frame — the segment's orientation
            if (par >= 0) memcpy(Mrot, &r->Mw[par * 16], sizeof Mrot);
            else m4_rot_norm(r->owner->world, Mrot);
        }
        // The bone frame must be WRITTEN: leaf children read it in this same
        // frame. Mw was never written before (calloc zeros) — every leaf got
        // a zero matrix, and skin on hands/feet collapsed to a point,
        // stretching the model into ribbons.
        memcpy(&r->Mw[i * 16], Mrot, sizeof(float) * 16);
        // W = T(pw) * Mrot; rest scale is brought back via the columns
        float W[16];
        memcpy(W, Mrot, sizeof W);
        W[12] = w->part[r->part[i]].pos[0];
        W[13] = w->part[r->part[i]].pos[1];
        W[14] = w->part[r->part[i]].pos[2];
        // Restore the node's rest scale (the reconstruction cut it off — pieces
        // of the model became wildly huge: inv_bind divides by the scale)
        float s = r->rsw[i];
        W[0] *= s; W[1] *= s; W[2]  *= s;
        W[4] *= s; W[5] *= s; W[6]  *= s;
        W[8] *= s; W[9] *= s; W[10] *= s;
        // And into model space (the instance matrix is factored out)
        m4mul(&p->node_world[i * 16], inv_inst, W);
    }
    // One-shot diagnostics: bones with a blown-off translation (at frame 200)
    if (!r->dbg_done) {
        r->dbg_calls++;
        if (r->dbg_calls < 200) return;
        r->dbg_done = 1;
        int bad = 0;
        for (int i = 0; i < r->nn && bad < 6; i++) {
            float *t = &p->node_world[i * 16 + 12];
            if (fabsf(t[0]) > 20.0f || fabsf(t[1]) > 20.0f ||
                fabsf(t[2]) > 20.0f) {
                SDL_Log("rag BAD bone %d (par %d cpart %d): t=%.1f %.1f %.1f",
                        i, a->nodes[i].parent,
                        i < r->nn ? r->cpart[i] : -1, t[0], t[1], t[2]);
                bad++;
            }
        }
        SDL_Log("rag scan: bad>=%d", bad);
    }
}

// Physics step for all enabled ragdolls (called by main.c in the fixed tick).
// After integration — JOINT LIMITS: the angle at a joint stays within rest±70°,
// otherwise elbows/knees bend through the body and limbs turn inside out.
// Limits move particles OUTSIDE the links, so physics_relax must follow
// them (we alternate 2 passes): links re-converge what the limits moved,
// otherwise the next frame's step drags particles back — ragdoll jitter and blow-up.
static void rag_joint_limits(AnimRag *r, const AnimGlb *a, PhysWorld *w)
{
    for (int k = 0; k < r->nn; k++) {
        int i = r->order[k];
        int par = a->nodes[i].parent;
        if (par < 0 || r->cpart[i] < 0) continue;
        const float *pp = w->part[r->part[par]].pos;
        const float *pi = w->part[r->part[i]].pos;
        const float *pc = w->part[r->cpart[i]].pos;
        float d1[3] = { pi[0]-pp[0], pi[1]-pp[1], pi[2]-pp[2] };
        float d2[3] = { pc[0]-pi[0], pc[1]-pi[1], pc[2]-pi[2] };
        float l1 = sqrtf(d1[0]*d1[0]+d1[1]*d1[1]+d1[2]*d1[2]);
        float l2 = sqrtf(d2[0]*d2[0]+d2[1]*d2[1]+d2[2]*d2[2]);
        if (l1 < 1e-5f || l2 < 1e-5f) continue;
        for (int q = 0; q < 3; q++) { d1[q] /= l1; d2[q] /= l2; }
        float cth = d1[0]*d2[0] + d1[1]*d2[1] + d1[2]*d2[2];
        if (cth > 1.0f) cth = 1.0f;
        if (cth < -1.0f) cth = -1.0f;
        float th = acosf(cth), th0 = r->tha0[i];
        float lo = th0 - 1.2f; if (lo < 0.1f) lo = 0.1f;
        float hi = th0 + 1.2f; if (hi > 2.6f) hi = 2.6f;
        if (th <= hi && th >= lo) continue;
        float tc = th > hi ? hi : lo;
        float dth = tc - th;                     // negative — toward the limit
        // axis = cross(d1, d2): rotation around it CHANGES the joint angle
        float n[3] = { d1[1]*d2[2] - d1[2]*d2[1],
                       d1[2]*d2[0] - d1[0]*d2[1],
                       d1[0]*d2[1] - d1[1]*d2[0] };
        float nl = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
        if (nl < 1e-6f) continue;                // joint fully extended
        n[0] /= nl; n[1] /= nl; n[2] /= nl;
        // Rodrigues: rotate vector v around n by dth
        float c = cosf(dth), s = sinf(dth), t = 1.0f - c;
        float v[3] = { pc[0] - pi[0], pc[1] - pi[1], pc[2] - pi[2] };
        float rv[3] = {
            (t*n[0]*n[0] + c)     *v[0] + (t*n[0]*n[1] - s*n[2])*v[1]
                + (t*n[0]*n[2] + s*n[1])*v[2],
            (t*n[1]*n[0] + s*n[2])*v[0] + (t*n[1]*n[1] + c)    *v[1]
                + (t*n[1]*n[2] - s*n[0])*v[2],
            (t*n[2]*n[0] - s*n[1])*v[0] + (t*n[2]*n[1] + s*n[0])*v[1]
                + (t*n[2]*n[2] + c)    *v[2]
        };
        float *dst = w->part[r->cpart[i]].pos;
        dst[0] = pi[0] + rv[0];
        dst[1] = pi[1] + rv[1];
        dst[2] = pi[2] + rv[2];
    }
}

void glb_anim_ragdoll_step_all(float dt)
{
    for (AnimRegEntry *e = s_anim_reg; e; e = e->next) {
        AnimParams *p = (AnimParams *)e->node->params;
        if (!p || !p->rag) continue;
        AnimRag *r = p->rag;
        AnimGlb *a = p->data;
        PhysWorld *w = r->w;
        physics_step(w, dt);
        for (int it = 0; it < 2; it++) {
            rag_joint_limits(r, a, w);
            physics_relax(w, 2);
        }
        // One-shot explosion diagnostics (step 200): a particle farther than
        // 30 m from the anchor point means the ragdoll is torn apart. Works
        // even if the model is off-camera (apply is not called).
        if (!r->dbg_done) {
            r->dbg_calls++;
            if (r->dbg_calls >= 200) {
                r->dbg_done = 1;
                int bad = 0;
                for (int i = 0; i < w->np; i++) {
                    float dx = w->part[i].pos[0] - w->part[r->origin_part].pos[0];
                    float dy = w->part[i].pos[1] - w->part[r->origin_part].pos[1];
                    float dz = w->part[i].pos[2] - w->part[r->origin_part].pos[2];
                    if (dx * dx + dy * dy + dz * dz > 30.0f * 30.0f) bad++;
                }
                // Skin scale drift: the length of a bone matrix column must
                // equal the rest scale; deviation = stretched model.
                float worst = 0.0f;
                for (int i = 0; i < r->nn; i++) {
                    const float *W = &p->node_world[i * 16];
                    float c0 = sqrtf(W[0]*W[0] + W[1]*W[1] + W[2]*W[2]);
                    if (r->rsw[i] > 1e-6f) {
                        float dev = fabsf(c0 / r->rsw[i] - 1.0f);
                        if (dev > worst) worst = dev;
                    }
                }
                SDL_Log("rag scan: %d of %d parts flying apart, "
                        "skin scale drift %.2f%%", bad, w->np, worst * 100.0f);
            }
        }
    }
}

void glb_anim_ragdoll_kick(SceneNode *n, const float vel[3])
{
    AnimParams *p = n ? (AnimParams *)n->params : NULL;
    if (!p || !p->rag) return;
    AnimRag *r = p->rag;
    for (int i = 0; i < r->w->np; i++)
        if (r->w->part[i].inv_mass > 0.0f)
            physics_impulse(r->w, i, vel);
}

int glb_anim_ragdoll_aim(float out_pos[3])
{
    if (!s_anim_reg) return 0;
    AnimParams *p = (AnimParams *)s_anim_reg->node->params;
    if (!p || !p->rag) return 0;
    PhysWorld *w = ((AnimRag *)p->rag)->w;
    int op = ((AnimRag *)p->rag)->origin_part;
    if (op < 0 || op >= w->np) return 0;
    out_pos[0] = w->part[op].pos[0];
    out_pos[1] = w->part[op].pos[1];
    out_pos[2] = w->part[op].pos[2];
    return 1;
}

void glb_anim_ragdoll_disable(SceneNode *n)
{
    AnimParams *p = n ? (AnimParams *)n->params : NULL;
    if (!p || !p->rag) return;
    AnimRag *r = p->rag;
    rag_free(r);
    p->rag = NULL;
}

// Debug: world positions of particles of ALL ragdolls (the renderer draws points).
// out_xyz — max*3 floats; returns the number of particles written.
int glb_anim_ragdoll_debug_particles(float *out_xyz, int max_parts)
{
    int n = 0;
    for (AnimRegEntry *e = s_anim_reg; e; e = e->next) {
        AnimParams *p = (AnimParams *)e->node->params;
        if (!p || !p->rag) continue;
        PhysWorld *w = ((AnimRag *)p->rag)->w;
        for (int i = 0; i < w->np && n < max_parts; i++) {
            out_xyz[n*3+0] = w->part[i].pos[0];
            out_xyz[n*3+1] = w->part[i].pos[1];
            out_xyz[n*3+2] = w->part[i].pos[2];
            n++;
        }
    }
    return n;
}

// Global node matrix from the working TRS (animated components live in wtrs)
static void node_global_mat(const AnimGlb*a,const float*wtrs,float*out,int idx){
    int chain[256],n=0,cur=idx;
    // Loop guarded against parent cycles (p==cur) — depth above 256 is truncated,
    // but such hierarchies are pathological; we stay conservative.
    while(cur>=0&&n<256){chain[n++]=cur; int p=a->nodes[cur].parent; if(p==cur)break; cur=p;}
    m4id(out);
    for(int i=n-1;i>=0;i--){int ni=chain[i]; AnimNode*N=&a->nodes[ni];
        float loc[16];
        if(wtrs&&!N->has_matrix){
            const float*w=&wtrs[ni*10];
            quat_mat(loc,w+3,w+7,w+0);   // trans, quat, scl
        } else memcpy(loc,N->trs,sizeof loc);
        float w[16]; m4mul(w,out,loc); memcpy(out,w,sizeof w); }
}

// Working buffer layout: wtrs[i*10] = trans[3] quat[4] scl[3]
#define WT_TR 0
#define WT_Q  3
#define WT_S  7
static void wtrs_base(const AnimGlb*a,float*wtrs){
    for(int i=0;i<a->nnode;i++){AnimNode*N=&a->nodes[i]; float*w=&wtrs[i*10];
        for(int q=0;q<3;q++){w[WT_TR+q]=N->btrans[q];w[WT_S+q]=N->bscl[q];}
        for(int q=0;q<4;q++)w[WT_Q+q]=N->bquat[q]; }
}
static void slerp(float*o,const float*a,const float*b,float f){
    float dot=0; for(int q=0;q<4;q++)dot+=a[q]*b[q];
    float qb[4]; memcpy(qb,b,sizeof qb);
    if(dot<0){dot=-dot;for(int q=0;q<4;q++)qb[q]=-qb[q];}
    float fa,qw=f;
    if(dot>0.9995f)fa=qw; else {float th=acosf(dot>1?1:(dot<-1?-1:dot)),st=sinf(th); fa=(st>1e-6f)?sinf(qw*th)/st:qw;}
    for(int q=0;q<4;q++)o[q]=a[q]*(1-fa)+qb[q]*fa;
}

// --- Sampler keyframe sampling -------------------------------------------------
// Value of the comp components at key k (for CUBICSPLINE — the "value" part of the triple).
static void keyval(const AnimSampler*S,int k,float*out){
    int i=(S->interp==ANIM_CUBIC)?(k*3+1)*S->comp:k*S->comp;
    memcpy(out,&S->values[i],(size_t)S->comp*sizeof(float));
}
// Indices of the neighboring keys and the segment parameter for time t.
static void seek(const AnimSampler*S,float t,int*k0,int*k1,float*u){
    int a=0,b=0; while(b<S->nkeys-1&&S->times[b]<t)b++; a=b>0?b-1:0;
    float seg=S->times[b]-S->times[a];
    float uu=(seg>1e-8f)?(t-S->times[a])/seg:0.f;
    *k0=a;*k1=b;*u=uu;
}
// translation/scale (comp=3): LINEAR (lerp) / STEP / CUBICSPLINE (Hermite).
static void anim_vec(const AnimSampler*S,float t,float*out){
    int k0,k1; float u; seek(S,t,&k0,&k1,&u);
    if(S->interp==ANIM_STEP){keyval(S,k1,out);return;}
    if(S->interp==ANIM_CUBIC){
        float p0[4],p1[4],m0[4],m1[4];
        keyval(S,k0,p0); keyval(S,k1,p1);
        // out-tangent of node k0, in-tangent of node k1
        memcpy(m0,&S->values[(k0*3+2)*S->comp],(size_t)S->comp*sizeof(float));
        memcpy(m1,&S->values[(k1*3+0)*S->comp],(size_t)S->comp*sizeof(float));
        float seg=S->times[k1]-S->times[k0];
        float u2=u*u,u3=u2*u;
        for(int q=0;q<S->comp;q++)
            out[q]=p0[q]*(2*u3-3*u2+1)+m0[q]*seg*(u3-2*u2+u)
                 +p1[q]*(-2*u3+3*u2)+m1[q]*seg*(u3-u2);
        return;
    }
    float p0[4],p1[4]; keyval(S,k0,p0); keyval(S,k1,p1);
    for(int q=0;q<S->comp;q++)out[q]=p0[q]+u*(p1[q]-p0[q]);
}
// rotation (comp=4): LINEAR = slerp, STEP, CUBICSPLINE = Hermite + renormalize.
static void anim_rot(const AnimSampler*S,float t,float*out){
    int k0,k1; float u; seek(S,t,&k0,&k1,&u);
    if(S->interp==ANIM_STEP){keyval(S,k1,out);return;}
    if(S->interp==ANIM_CUBIC){
        float tmp[4]; anim_vec(S,t,tmp);
        float l=0;for(int q=0;q<4;q++)l+=tmp[q]*tmp[q];l=sqrtf(l);
        if(l>1e-6f)for(int q=0;q<4;q++)out[q]=tmp[q]/l;
        else memcpy(out,tmp,sizeof tmp);
        return;
    }
    float p0[4],p1[4]; keyval(S,k0,p0); keyval(S,k1,p1); slerp(out,p0,p1,u);
}

// Build global node matrices with animation applied, plus jointMatrix
static void build_bones(AnimParams*p){
    AnimGlb*a=p->data; if(!a->nodes)return;
    wtrs_base(a,p->wtrs);   // working TRS = base
    // 1) apply the active clip to the nodes' working TRS
    if(a->nclip>0){
        int cl=p->cur_clip<a->nclip?p->cur_clip:0;
        AnimClip*C=&a->clips[cl]; float t=p->anim_time;
        if(C->duration>0){
            if(p->loop){ while(t>C->duration)t-=C->duration; while(t<0)t+=C->duration; }
            else if(t>C->duration)t=C->duration;
        }
        for(int h=0;h<C->nch;h++){
            AnimChannel*Ch=&C->channels[h]; if(Ch->node<0||Ch->node>=a->nnode)continue;
            if(Ch->sampler<0||Ch->sampler>=C->ns)continue;
            AnimSampler*S=&C->samplers[Ch->sampler]; if(!S->nkeys)continue;
            float*w=&p->wtrs[Ch->node*10];
            if(Ch->path==1)      anim_rot(S,t,w+WT_Q);         // rotation
            else                 anim_vec(S,t,w+(Ch->path==0?WT_TR:WT_S)); // trans/scale
        }
    }
    // 2) global node matrices from working TRS
    for(int i=0;i<a->nnode;i++){float m[16]; node_global_mat(a,p->wtrs,m,i);
        memcpy(&p->node_world[i*16],m,sizeof m); }
    // 2.5) RAGDOLL: physics particles override the animated pose —
    //      node_world is built from the particles (bones follow physics),
    //      step 3 below already takes the ragdoll pose.
    if(p->rag) ragdoll_apply(p);
    // 3) jointMatrix per skin, spread over primitives (per-primitive remap).
    //    Vertices are skinned in the shared skeleton space, so skinned
    //    primitives do NOT need node_world (it is already inside jointMatrix);
    //    non-skinned children of the hierarchy take it from the node (see anim_render).
    for(int s=0;s<a->nskin;s++){
        AnimSkin*S=&a->skins[s]; int n=S->joint_count; if(n>p->skin_cap)n=p->skin_cap;
        for(int j=0;j<n;j++){int nodeidx=S->joints[j];
            if(nodeidx<0||nodeidx>=a->nnode){m4id(&p->skin[j*16]);continue;}
            float jm[16]; m4mul(jm,&p->node_world[nodeidx*16],&S->inv_bind[j*16]);
            memcpy(&p->skin[j*16],jm,sizeof jm); }
        for(int i=0;i<p->count;i++){
            AnimDrawItem*d=&p->items[i];
            if(d->skin!=s) continue;
            for(int j=0;j<d->joint_n;j++){
                int g=d->joint_map[j];
                if(g<0||g>=n) m4id(&d->bone[j*16]);
                else memcpy(&d->bone[j*16],&p->skin[g*16],16*sizeof(float));
            }
        }
    }
}

static void anim_update(SceneNode*n,float dt){
    AnimParams*p=n->params; if(!p||!p->data)return;
    if(p->moving){
        float dx=p->dest[0]-n->pos[0], dy=p->dest[1]-n->pos[1], dz=p->dest[2]-n->pos[2];
        float dist=sqrtf(dx*dx+dy*dy+dz*dz);
        float step=p->move_speed>0?p->move_speed*dt:0;
        if(step>=dist) { n->pos[0]=p->dest[0]; n->pos[1]=p->dest[1]; n->pos[2]=p->dest[2]; p->moving=0; }
        else if(dist>1e-6f){ float k=step/dist;
            n->pos[0]+=dx*k; n->pos[1]+=dy*k; n->pos[2]+=dz*k; }
    }
    if(p->enabled && !p->paused) p->anim_time+=dt*p->anim_speed;
    if(p->enabled) build_bones(p);
}
static void anim_render(SceneNode*n,float dt){
    (void)dt;
    AnimParams*p=n?n->params:NULL; if(!p||!p->enabled)return;
    if(p->rc->pass==RP_SHADOW){
        // Shadow of the animated model in the current frame's pose (skinning in VS)
        if(!s_anim_shadow_prog){
            s_anim_shadow_prog=resources_program_replace(
                0,"shaders/skinned_shadow.vs","shaders/mesh_shadow.fs");
            anim_shadow_init();
            shader_on_reshade(anim_shadow_reshade);
        }
        glEnable(GL_DEPTH_TEST); glUseProgram(s_anim_shadow_prog);
        glUniformMatrix4fv(s_as_vp,1,GL_FALSE,p->rc->shadow_vp);
        glUniform1i(s_as_at,1);
        if(s_as_tex>=0)glUniform1i(s_as_tex,0);
        glActiveTexture(GL_TEXTURE0);
        for(int i=0;i<p->count;i++){
            AnimDrawItem*d=&p->items[i];
            if(p->rag&&d->rag_skip)continue;   // GROUND plane and other junk
            float model[16];
            if(!d->has_skin&&p->data&&d->node>=0&&d->node<p->data->nnode)
                m4mul(model,&p->node_world[d->node*16],n->world);
            else
                memcpy(model,n->world,sizeof model);
            glUniformMatrix4fv(s_as_model,1,GL_FALSE,model);
            if(s_as_hasskin>=0)glUniform1i(s_as_hasskin,d->has_skin?1:0);
            if(d->has_skin&&s_as_bones>=0)glUniformMatrix4fv(s_as_bones,d->joint_n,GL_FALSE,d->bone);
            glBindTexture(GL_TEXTURE_2D,d->tex);
            glBindVertexArray(d->vao);
            glDrawElements(GL_TRIANGLES,d->index_count,d->index_type,NULL);
        }
        glBindVertexArray(0);
        return;
    }
    if(!frustum_test_sphere(&p->rc->frustum, n->world[12], n->world[13], n->world[14], p->radius))
        return;
    // build_bones already ran in anim_update for this frame — do not duplicate it.
    // Base instance matrix = the node's WORLD matrix (scene.c): it carries
    // position, rotation, scale and — if the node is attached — the parent
    // bone's matrix. The model can be rotated and hung on other models.
    // SKINNED primitives are drawn in it: jointMatrix = world[joint]*inverseBind
    // is already in skeleton space. NON-SKINNED meshes (props in the hierarchy)
    // are additionally multiplied by their own node's node_world — otherwise
    // they sit in the file's "raw" scale.
    const float *inst = n->world;
    glEnable(GL_DEPTH_TEST); glUseProgram(p->prog);
    scene_uniforms_bind(&p->su,p->rc);
    scene_uniforms_probe(&p->su, inst[12], inst[13], inst[14]);
    if(p->u_tex>=0)glUniform1i(p->u_tex,0);
    for(int i=0;i<p->count;i++){
        AnimDrawItem*d=&p->items[i];
        // In ragdoll, non-skeletal primitives (the model's built-in GROUND plane
        // and other "non body parts") are not drawn: their nodes get dragged by
        // physics and unfurl into giant sails.
        if(p->rag&&d->rag_skip)continue;
        float model[16];
        if(!d->has_skin&&p->data&&d->node>=0&&d->node<p->data->nnode)
            m4mul(model,&p->node_world[d->node*16],inst);
        else
            memcpy(model,inst,sizeof model);
        glUniformMatrix4fv(p->u_model,1,GL_FALSE,model);
        if(p->u_specular>=0)glUniform1f(p->u_specular,d->specular);
        if(p->u_hasskin>=0)glUniform1i(p->u_hasskin,d->has_skin?1:0);
        if(d->has_skin&&p->u_bones>=0)glUniformMatrix4fv(p->u_bones,d->joint_n,GL_FALSE,d->bone);
        if(p->u_uvtile>=0)glUniform2f(p->u_uvtile,d->mat.tile[0],d->mat.tile[1]);
        material_bind(&d->mat,&d->ml,0);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,d->tex);
        glBindVertexArray(d->vao);
        glDrawElements(GL_TRIANGLES,d->index_count,d->index_type,NULL);
    }
    glBindVertexArray(0);
}

// --- Attachment points: bone matrix (scene.h hook) --------------------------------
// Slot = glTF NODE index. We hand out node_world[slot] — the bone matrix in
// MODEL space (without its world matrix: scene.c multiplies that itself).
// The model scale (p->scale) is already in the node's world matrix, while
// node_world carries only the file's internal hierarchy — so the chain comes
// out world(model) * bone(model) * local(child), as expected.
static void anim_attach_matrix(SceneNode *parent, int slot, float out[16])
{
    AnimParams *p = parent ? parent->params : NULL;
    if (!p || !p->data || !p->node_world) return;
    if (slot < 0 || slot >= p->data->nnode) return;
    memcpy(out, &p->node_world[slot * 16], 16 * sizeof(float));
}

int glb_anim_bone_count(SceneNode *n)
{
    AnimParams *p = n && n->type && strcmp(n->type, "AnimInstance3D") == 0
                  ? n->params : NULL;
    return (p && p->data) ? p->data->nnode : 0;
}

const char *glb_anim_bone_name(SceneNode *n, int idx)
{
    AnimParams *p = n && n->type && strcmp(n->type, "AnimInstance3D") == 0
                  ? n->params : NULL;
    if (!p || !p->data || idx < 0 || idx >= p->data->nnode) return NULL;
    return p->data->nodes[idx].name;
}

int glb_anim_bone_find(SceneNode *n, const char *name)
{
    AnimParams *p = n && n->type && strcmp(n->type, "AnimInstance3D") == 0
                  ? n->params : NULL;
    if (!p || !p->data || !name || !name[0]) return -1;
    // Exact name match
    for (int i = 0; i < p->data->nnode; i++)
        if (strcmp(p->data->nodes[i].name, name) == 0) return i;
    // Otherwise — substring match (in GLB bones are often named "mixamorig:Hand_R")
    for (int i = 0; i < p->data->nnode; i++)
        if (p->data->nodes[i].name[0] && strstr(p->data->nodes[i].name, name))
            return i;
    return -1;
}

static AnimDrawItem*build_item(const AnimPrim*P,GLuint prog,int has_skin,int skin,int node,const float*snow){
    AnimDrawItem*d=calloc(1,sizeof* d);
    if(!d) return NULL;
    d->skin=skin; d->has_skin=has_skin; d->node=node;
    snprintf(d->mtl_name,sizeof d->mtl_name,"%s",P->matname);
    // per-primitive remap: which skin bones are actually used (≤ MAX_BONES).
    // Bone indices (JOINTS_0) are stored as int — the component may be ubyte
    // (0..255) or ushort (0..65535, Blender/Sketchfab-FBX). Truncating to byte
    // is FORBIDDEN: an index >255 gets clipped, and vertices bind to foreign bones.
    int used[MAX_BONES]; int n=0, overflow=0;
    if(has_skin){for(int i=0;i<P->vcount;i++)for(int k=0;k<4;k++){
        int b=P->bone_idx[4*i+k];
        if(b<0) continue;
        int known=-1;
        for(int u=0;u<n;u++) if(used[u]==b){known=u;break;}
        if(known<0){
            if(n<MAX_BONES){ used[n]=b; n++; }
            else overflow=1;
        } }
    }
    if(has_skin&&overflow)
        SDL_Log("glb_anim: primitive uses >=%d bones — some vertices bound to bone 0",MAX_BONES);
    d->joint_n=n;
    if(has_skin&&n>0){
        d->joint_map=malloc((size_t)n*sizeof(int));
        memcpy(d->joint_map,used,(size_t)n*sizeof(int));
        d->bone=malloc((size_t)n*16*sizeof(float)); m4id(d->bone);
    }
    glGenVertexArrays(1,&d->vao); glBindVertexArray(d->vao);
    glGenBuffers(1,&d->vbo); glBindBuffer(GL_ARRAY_BUFFER,d->vbo);
    const size_t stride=(has_skin?16:8)*sizeof(float);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)((size_t)P->vcount*stride),NULL,GL_STATIC_DRAW);
    float*cpu=malloc((size_t)P->vcount*(has_skin?16:8)*sizeof(float));
    for(int i=0;i<P->vcount;i++){
        cpu[(has_skin?16:8)*i+0]=P->verts[3*i+0]; cpu[(has_skin?16:8)*i+1]=P->verts[3*i+1]; cpu[(has_skin?16:8)*i+2]=P->verts[3*i+2];
        cpu[(has_skin?16:8)*i+3]=P->normals[3*i+0]; cpu[(has_skin?16:8)*i+4]=P->normals[3*i+1]; cpu[(has_skin?16:8)*i+5]=P->normals[3*i+2];
        cpu[(has_skin?16:8)*i+6]=P->uvs[2*i+0]; cpu[(has_skin?16:8)*i+7]=P->uvs[2*i+1];
        if(has_skin){ for(int k=0;k<4;k++){
                int b=P->bone_idx[4*i+k],loc=0;
                for(int u=0;u<n;u++) if(used[u]==b){loc=u;break;}
                cpu[(has_skin?16:8)*i+8+k]=(float)loc; }
            for(int k=0;k<4;k++)cpu[(has_skin?16:8)*i+12+k]=P->bone_w[4*i+k]; }
    }
    glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)((size_t)P->vcount*stride),cpu);
    free(cpu);
    GLint a_pos=glGetAttribLocation(prog,"aPos"),a_nrm=glGetAttribLocation(prog,"aNormal"),a_uv=glGetAttribLocation(prog,"aUV");
    GLint a_bi=glGetAttribLocation(prog,"aBoneIndices"),a_bw=glGetAttribLocation(prog,"aBoneWeights");
    glVertexAttribPointer(a_pos,3,GL_FLOAT,GL_FALSE,(GLsizei)stride,NULL); glEnableVertexAttribArray(a_pos);
    if(a_nrm>=0){glVertexAttribPointer(a_nrm,3,GL_FLOAT,GL_FALSE,(GLsizei)stride,(void*)(3*sizeof(float)));glEnableVertexAttribArray(a_nrm);}
    if(a_uv>=0){glVertexAttribPointer(a_uv,2,GL_FLOAT,GL_FALSE,(GLsizei)stride,(void*)(6*sizeof(float)));glEnableVertexAttribArray(a_uv);}
    if(has_skin&&a_bi>=0){glVertexAttribPointer(a_bi,4,GL_FLOAT,GL_FALSE,(GLsizei)stride,(void*)(8*sizeof(float)));glEnableVertexAttribArray(a_bi);}
    if(has_skin&&a_bw>=0){glVertexAttribPointer(a_bw,4,GL_FLOAT,GL_FALSE,(GLsizei)stride,(void*)(12*sizeof(float)));glEnableVertexAttribArray(a_bw);}
    if(snow){
        glGenBuffers(1,&d->snow_vbo); glBindBuffer(GL_ARRAY_BUFFER,d->snow_vbo);
        glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)((size_t)P->vcount*sizeof(float)),snow,GL_STATIC_DRAW);
        GLint a_snow=glGetAttribLocation(prog,"aSnow");
        if(a_snow>=0){glVertexAttribPointer(a_snow,1,GL_FLOAT,GL_FALSE,0,NULL);glEnableVertexAttribArray(a_snow);}
    }
    glGenBuffers(1,&d->ebo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,d->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,(GLsizeiptr)((size_t)P->index_count*sizeof(unsigned int)),P->indices,GL_STATIC_DRAW);
    d->index_count=P->index_count; d->index_type=GL_UNSIGNED_INT;
    glBindVertexArray(0); glBindBuffer(GL_ARRAY_BUFFER,0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,0);
    d->has_skin=has_skin;
    material_defaults(&d->mat); memcpy(d->mat.tint,P->tint,sizeof P->tint);
    d->mat.alpha=P->alpha; d->mat.blend=P->blend; d->mat.cull=P->cull;
    d->mat.tile[0]=P->tile[0]; d->mat.tile[1]=P->tile[1]; d->mat.set=1;
    material_locs_init(&d->ml,prog); d->specular=P->specular;
    if(P->img){ glGenTextures(1,&d->tex); d->tex_owned=1; glBindTexture(GL_TEXTURE_2D,d->tex);
        GLint wm=GL_REPEAT,fm=GL_LINEAR_MIPMAP_LINEAR,fmag=GL_LINEAR;
        if(P->wrap==MAT_WRAP_CLAMP)wm=GL_CLAMP_TO_EDGE; else if(P->wrap==MAT_WRAP_MIRROR)wm=GL_MIRRORED_REPEAT;
        if(P->filter==MAT_FILTER_NEAREST){fm=GL_NEAREST;fmag=GL_NEAREST;}
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,wm); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,wm);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,fm); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,fmag);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,P->iw,P->ih,0,GL_RGBA,GL_UNSIGNED_BYTE,P->img); glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D,0); }
    else d->tex=resources_texture("img/notfound.png",4,GL_REPEAT,0);   // tex_owned=0 (cache)
    return d;
}

SceneNode *glb_anim_make_node(const char*path,const float pos[3],float scale,const RendererContext*rc){
    // Once: for an AnimInstance3D node leaving the tree (node_free_tree sends
    // EV_NODE_EXIT BEFORE free(params)) all GL objects and data are freed.
    static int s_registered=0;
    if(!s_registered){ events_listen(EV_NODE_EXIT,anim_node_exit,NULL); s_registered=1; }
    AnimGlb*a=glb_anim_parse(path);
    if(!a)return NULL;
    GLuint prog=make_program("shaders/skinned.vs","shaders/mesh.fs");
    if(!prog){glb_anim_free(a);return NULL;}
    AnimParams*p=calloc(1,sizeof* p);
    if(!p){ glb_anim_free(a); return NULL; }
    p->rc=rc; p->scale=scale>0?scale:1; p->prog=prog; p->data=a;
    snprintf(p->path,sizeof p->path,"%s",path); p->enabled=1;
    scene_uniforms_init(&p->su,prog);
    p->u_model=glGetUniformLocation(prog,"uModel"); p->u_tex=glGetUniformLocation(prog,"uTex");
    p->u_specular=glGetUniformLocation(prog,"uSpecular"); p->u_bones=glGetUniformLocation(prog,"uBones");
    p->u_hasskin=glGetUniformLocation(prog,"uHasSkin"); p->u_uvtile=glGetUniformLocation(prog,"uUvTile");
    p->items=calloc((size_t)(a->ninst?a->ninst:1),sizeof* p->items);
    p->count=p->items?a->ninst:0;
    p->radius=0.0f;
    // Per-vertex "open to the sky above" over the whole model (all primitives of the file):
    // roof/hood get snow, floor/seats under the roof — do not.
    const float**vp=malloc((size_t)(a->nprim?a->nprim:1)*sizeof*vp);
    int*vc=malloc((size_t)(a->nprim?a->nprim:1)*sizeof*vc);
    const unsigned**ip=malloc((size_t)(a->nprim?a->nprim:1)*sizeof*ip);
    int*ic=malloc((size_t)(a->nprim?a->nprim:1)*sizeof*ic);
    int snow_total=0;
    float*snow_mask=NULL;
    if(vp&&vc&&ip&&ic){
        for(int pr=0;pr<a->nprim;pr++){
            vp[pr]=a->prims[pr].verts; vc[pr]=a->prims[pr].vcount;
            ip[pr]=a->prims[pr].indices; ic[pr]=a->prims[pr].index_count;
        }
        snow_mask=snow_mask_compute(vp,vc,ip,ic,a->nprim,&snow_total);
    }
    free(vp); free(vc); free(ip); free(ic);
    int*prim_off=malloc((size_t)(a->nprim?a->nprim:1)*sizeof*prim_off);
    if(prim_off){ int acc=0; for(int pr=0;pr<a->nprim;pr++){ prim_off[pr]=acc; acc+=a->prims[pr].vcount; } }
    for(int i=0;i<p->count;i++){
        AnimInstance*it=&a->instances[i]; AnimPrim*P=&a->prims[it->prim];
        int hs=(P->bone_idx&&it->skin>=0&&it->skin<a->nskin)?1:0;
        AnimDrawItem*bi=build_item(P,prog,hs,it->skin,it->node,
                                   (snow_mask&&prim_off)? snow_mask+prim_off[it->prim] : NULL);
        if(!bi) continue;
        p->items[i]=*bi; free(bi);   // struct copied — free the wrapper
        for(int v=0;v<P->vcount;v++){
            float dx=P->verts[3*v+0], dy=P->verts[3*v+1], dz=P->verts[3*v+2];
            float r2=dx*dx+dy*dy+dz*dz; if(r2>p->radius)p->radius=r2;
        }
    }
    free(prim_off); free(snow_mask);
    p->radius=sqrtf(p->radius)*p->scale;
    p->node_world=malloc((size_t)(a->nnode? a->nnode:1)*16*sizeof(float));
    p->wtrs=calloc((size_t)(a->nnode? a->nnode:1)*10,sizeof(float));
    int jc=0; for(int s=0;s<a->nskin;s++)if(a->skins[s].joint_count>jc)jc=a->skins[s].joint_count;
    p->skin=malloc((size_t)(jc?jc:1)*16*sizeof(float)); m4id(p->skin);
    p->skin_cap=jc; p->anim_time=0; p->anim_speed=1; p->paused=0; p->loop=1; p->cur_clip=0;
    glUseProgram(prog); if(p->u_tex>=0)glUniform1i(p->u_tex,0);
    for(int c=0;c<SHADOW_CASCADES;c++){if(p->su.u_shadowmap[c]>=0)glUniform1i(p->su.u_shadowmap[c],3+c);
        if(p->su.u_shadowdepth[c]>=0)glUniform1i(p->su.u_shadowdepth[c],3+c);}
    glUseProgram(0);
    float size[3]={scale,scale,scale};
    SceneNode*n=node_new(path,"AnimInstance3D",pos,size,p,anim_update,anim_render);
    // Scale — in the node transform (scene.c will build the world matrix from it).
    n->scl[0]=n->scl[1]=n->scl[2]=p->scale;
    n->attach_fn=anim_attach_matrix;      // nodes can be attached to bones
    build_bones(p);
    node_update_transforms(n);            // world matrix before the first render
    anim_reg_add(n);
    return n;
}

// --- Release of node resources (called on EV_NODE_EXIT) ---------------------
// node_free_tree itself frees only the AnimParams struct; everything inside it
// (GPU buffers, textures, program from the cache, model data) is handled here.
static void anim_free_resources(SceneNode*n){
    AnimParams*p=n?n->params:NULL; if(!p)return;
    anim_reg_remove(n);
    glb_anim_ragdoll_disable(n);
    if(p->items){
        for(int i=0;i<p->count;i++){
            AnimDrawItem*d=&p->items[i];
            glDeleteVertexArrays(1,&d->vao);
            glDeleteBuffers(1,&d->vbo);
            glDeleteBuffers(1,&d->snow_vbo);
            glDeleteBuffers(1,&d->ebo);
            if(d->tex_owned) glDeleteTextures(1,&d->tex);
            else resources_texture_release(d->tex);
            free(d->joint_map); free(d->bone);
        }
        free(p->items); p->items=NULL;
    }
    if(p->prog){ resources_program_release(p->prog); p->prog=0; }
    free(p->node_world); p->node_world=NULL;
    free(p->wtrs); p->wtrs=NULL;
    free(p->skin); p->skin=NULL;
    glb_anim_free(p->data); p->data=NULL;
}

static void anim_node_exit(const Event*e,void*user){
    (void)user;
    if(!e->a)return;
    const SceneNode*n=e->a;
    if(n->type&&strcmp(n->type,"AnimInstance3D")==0)
        anim_free_resources((SceneNode*)n);
}

void glb_anim_node_render(SceneNode*n,float dt){ anim_render(n,dt); }

// --- Animation clips ------------------------------------------------------------
// All functions in this section work ONLY with an AnimInstance3D node (AnimParams).
// For a static GlbInstance3D, params is a GlbParams without a data field: reading
// p->data would be garbage/SEGV. Check the node type and parameters up front.
static AnimParams*anim_params_of(SceneNode*n){
    if(!n||!n->type||strcmp(n->type,"AnimInstance3D")!=0)return NULL;
    return n->params;
}
int glb_anim_clip_count(SceneNode*n){
    AnimParams*p=anim_params_of(n);
    return p&&p->data?p->data->nclip:0;
}
const char*glb_anim_clip_name(SceneNode*n,int idx){
    AnimParams*p=anim_params_of(n);
    if(!p||!p->data||idx<0||idx>=p->data->nclip)return NULL;
    return p->data->clips[idx].name[0]?p->data->clips[idx].name:NULL;
}
int glb_anim_clip_find(SceneNode*n,const char*name){
    AnimParams*p=anim_params_of(n);
    if(!p||!p->data||!name)return -1;
    for(int i=0;i<p->data->nclip;i++)
        if(p->data->clips[i].name[0]&&!strcmp(p->data->clips[i].name,name))return i;
    return -1;
}
void glb_anim_set_clip(SceneNode*n,int idx){
    AnimParams*p=anim_params_of(n);
    if(!p||!p->data||p->data->nclip<=0)return;
    p->cur_clip=idx<0?0:(idx>=p->data->nclip?p->data->nclip-1:idx);
    p->anim_time=0;   // restart the clip from the beginning
    build_bones(p);
}
const char*glb_anim_current_clip(SceneNode*n){
    AnimParams*p=anim_params_of(n);
    if(!p||!p->data||p->cur_clip<0||p->cur_clip>=p->data->nclip)return NULL;
    return p->data->clips[p->cur_clip].name[0]?p->data->clips[p->cur_clip].name:NULL;
}
void glb_anim_set_speed(SceneNode*n,float speed){
    AnimParams*p=anim_params_of(n); if(!p)return;
    p->anim_speed=speed;
}
void glb_anim_set_paused(SceneNode*n,int paused){
    AnimParams*p=anim_params_of(n); if(!p)return;
    p->paused=paused?1:0;
}
void glb_anim_set_loop(SceneNode*n,int loop){
    AnimParams*p=anim_params_of(n); if(!p)return;
    p->loop=loop?1:0;
}
void glb_anim_set_time(SceneNode*n,float t){
    AnimParams*p=anim_params_of(n); if(!p)return;
    AnimGlb*a=p->data; float d=0;
    if(a&&p->cur_clip>=0&&p->cur_clip<a->nclip)d=a->clips[p->cur_clip].duration;
    if(t<0)t=0;
    if(d>0) t = p->loop ? fmodf(t,d) : (t>d?d:t);
    p->anim_time=t;
    build_bones(p);   // update the pose immediately (not waiting for a frame)
}
float glb_anim_time(SceneNode*n){
    AnimParams*p=anim_params_of(n);
    return p?p->anim_time:0;
}
float glb_anim_scale_get(SceneNode*n){
    AnimParams*p=anim_params_of(n); return p?p->scale:1;
}
int glb_anim_paused_get(SceneNode*n){
    AnimParams*p=anim_params_of(n); return p?p->paused:0;
}
int glb_anim_loop_get(SceneNode*n){
    AnimParams*p=anim_params_of(n); return p?p->loop:0;
}
float glb_anim_speed_get(SceneNode*n){
    AnimParams*p=anim_params_of(n); return p?p->anim_speed:1;
}
int glb_anim_count(void){
    int c=0; for(AnimRegEntry*e=s_anim_reg;e;e=e->next)c++; return c;
}
SceneNode*glb_anim_by_index(int idx){ return anim_reg_find(idx); }
const char*glb_anim_path(SceneNode*n){
    AnimParams*p=anim_params_of(n); return p?p->path:(const char*)NULL;
}
void glb_anim_set_enabled(SceneNode*n,int on){
    AnimParams*p=anim_params_of(n); if(!p)return;
    p->enabled=on?1:0;
}
int glb_anim_enabled(SceneNode*n){
    AnimParams*p=anim_params_of(n); return p?p->enabled:0;
}
void glb_anim_set_pos(SceneNode*n,const float pos[3]){
    if(!n||!anim_params_of(n))return;
    n->pos[0]=pos[0]; n->pos[1]=pos[1]; n->pos[2]=pos[2];
}
void glb_anim_move_to(SceneNode*n,const float pos[3],float speed){
    AnimParams*p=anim_params_of(n); if(!p)return;
    p->dest[0]=pos[0]; p->dest[1]=pos[1]; p->dest[2]=pos[2];
    p->move_speed=speed>0?speed:1; p->moving=1;
}
void glb_anim_stop_move(SceneNode*n){
    AnimParams*p=anim_params_of(n); if(!p)return; p->moving=0;
}
void glb_anim_set_scale(SceneNode*n,float s){
    AnimParams*p=anim_params_of(n); if(!p)return;
    if(s<=0)s=1;
    p->scale=s;
    // Scale now lives in the node TRANSFORM (scene.c builds the world matrix
    // from it) — p->scale stays for the cull radius and API queries.
    n->scl[0]=n->scl[1]=n->scl[2]=s;
    n->size[0]=n->size[1]=n->size[2]=s;
    if(p->data){float r=0;
        for(int i=0;i<p->data->ninst;i++){AnimPrim*P=&p->data->prims[p->data->instances[i].prim];
            for(int v=0;v<P->vcount;v++){
                float dx=P->verts[3*v],dy=P->verts[3*v+1],dz=P->verts[3*v+2];
                float r2=dx*dx+dy*dy+dz*dz; if(r2>r)r=r2; }}
        p->radius=sqrtf(r)*s;
    }
}
void glb_anim_remove_node(SceneNode*n){
    if(!n||!anim_params_of(n))return;
    node_remove(n);
    node_free_tree(n);   // sends EV_NODE_EXIT -> anim_free_resources (unreg+GL)
}
