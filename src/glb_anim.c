// ============================================================================
//  glb_anim.c — скелетная анимация (skinning) из бинарного glTF 2.0 (.glb)
// ----------------------------------------------------------------------------
//  Два слоя:
//    glb_anim_parse()  — CPU: типы (accessors/буферы), иерархия узлов, кожи
//                        (joints + inverse bind matrices), примитивы с весами
//                        (JOINTS_0/WEIGHTS_0), клипы анимации (channels+samplers).
//                        Геометрия остаётся в ПОЗЕ ПРИВЯЗКИ.
//    glb_anim_make_node() — строит GPU-буферы/текстуры, программу skinned.vs +
//                        mesh.fs, узел с update (тикает время) и render (скиннит).
//
//  Математика костей (glTF 2.0 §4.8 / Appendix C):
//    jointMatrix(j) = globalTransformOfJointNode(j) * inverseBindMatrix(j)
//    skinnedVertex  = w0*J0*v + w1*J1*v + ...  (до 4 костей на вершину)
//  Сумма весов нормируется делением в шейдере (или коррекцией здесь).
// ============================================================================
#include "glb_anim.h"

#include "events.h"
#include "json.h"
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

// Какой набор TEXCOORD использует baseColorTexture материала (0 по умолчанию).
// glTF разрешает до двух развёрток на примитив: лица/глаза персонажей часто
// живут в TEXCOORD_1 — читать всегда только нулевой набор = сломанные текстуры.
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

// --- Тени скиннед-моделей -----------------------------------------------------
// Глубинный проход (skinned_shadow.vs + mesh_shadow.fs): та же кожа, что в
// цветном проходе — модель отбрасывает тень В ПОЗЕ ТЕКУЩЕГО КАДРА. Программа
// одна на все anim-узлы (модульный статик, hot-reload через reshade-хук).
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
// quat (x,y,z,w) -> матрица
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
// Безопасное чтение индекса: только беззнаковые SCALAR-типы, диапазон [0,maxIdx].
static long rfi(const unsigned char*bin,long base,const GA*a,int elem,long maxIdx){
    if(a->vec!=1) return -1;
    long idx=-1; int sz=cs(a->comp); const unsigned char*p=bin+base+(long)elem*a->stride;
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

// Акцессор укладывается в свой bufferView и в bin-чанк (защита от битого/
// вредоносного GLB: смещения/длины берутся из JSON и ДО чтения обязаны быть
// проверены против реального размера файла). Шаг чтения rvv/rfv = a->stride.
static int acc_fits(const GA*a,const GB*bvs,int bv_n,long bin_len){
    if(!a||a->count<=0)return 0;
    if(a->bufferView<0||a->bufferView>=bv_n)return 0;
    const GB*bv=&bvs[a->bufferView];
    int sz=cs(a->comp)*a->vec; if(sz<=0)return 0;
    if(a->count>bin_len)return 0;
    // Для интерлевинга: реальный размер = (count-1)*stride + sz
    long stride = a->stride > 0 ? a->stride : sz;
    long need = (a->count - 1) * stride + sz;
    long base = bv->byteOffset + a->byteOffset;
    return base>=0&&bv->byteLength>=0&&need<=bv->byteLength&&base+need<=bin_len;
}
// bufferView (картинка/сырые данные) целиком внутри bin-чанка
static int bv_fits(const GB*bvs,int bv_n,int i,long bin_len){
    if(i<0||i>=bv_n)return 0;
    return bvs[i].byteOffset>=0&&bvs[i].byteLength>=0&&
           bvs[i].byteOffset+bvs[i].byteLength<=bin_len;
}

// --- Сам парсер ----------------------------------------------------------------
AnimGlb *glb_anim_parse(const char *path)
{
    unsigned int filesz=0;
    unsigned char*file=resources_read_file(path,&filesz);
    if(!file){ SDL_Log("glb_anim: не найден %s",path); return NULL; }
    if(filesz<20||memcmp(file,"glTF",4)){ SDL_Log("glb_anim: %s не GLB",path); free(file); return NULL; }
    const unsigned char*json_data=NULL,*bin=NULL; unsigned int json_len=0,bin_len=0;
    unsigned int off=12;
    while(off+8<=filesz){ unsigned int clen,ctype; memcpy(&clen,file+off,4); memcpy(&ctype,file+off+4,4); off+=8;
        if(off+clen>filesz)break;
        if(ctype==0x4E4F534A){json_data=file+off;json_len=clen;}
        else if(ctype==0x004E4942){bin=file+off;bin_len=clen;}
        off+=clen; }
    if(!json_data||!bin){ SDL_Log("glb_anim: %s нет чанков",path); free(file); return NULL; }
    const JsonNode*root=json_parse((const char*)json_data,(size_t)json_len);
    if(!root){ SDL_Log("glb_anim: %s JSON не распознан",path); free(file); return NULL; }

    const JsonNode*acc_a=json_find(root,"accessors"),*bv_a=json_find(root,"bufferViews");
    const JsonNode*nd_a=json_find(root,"nodes"),*ms_a=json_find(root,"meshes");
    const JsonNode*mt_a=json_find(root,"materials"),*tx_a=json_find(root,"textures");
    const JsonNode*im_a=json_find(root,"images"),*sm_a=json_find(root,"samplers");
    const JsonNode*sk_a=json_find(root,"skins"),*an_a=json_find(root,"animations");
    int acc_n=acc_a&&acc_a->type==JSON_ARRAY?json_array_len(acc_a):0;
    int bv_n=bv_a&&bv_a->type==JSON_ARRAY?json_array_len(bv_a):0;
    int node_n=nd_a&&nd_a->type==JSON_ARRAY?json_array_len(nd_a):0;
    if(node_n==0){ SDL_Log("glb_anim: %s нет nodes",path); json_free(); free(file); return NULL; }

    GB* bvs=calloc((size_t)(bv_n?bv_n:1),sizeof* bvs);
    for(int i=0;i<bv_n;i++){const JsonNode*b=json_array_get(bv_a,i);
        bvs[i].byteOffset=(long)json_num(json_find(b,"byteOffset"),0);
        bvs[i].byteLength=(long)json_num(json_find(b,"byteLength"),0);
        bvs[i].byteStride=(int)json_num(json_find(b,"byteStride"),0);}
    GA* accs=calloc((size_t)(acc_n?acc_n:1),sizeof* accs);
    for(int i=0;i<acc_n;i++){const JsonNode*a=json_array_get(acc_a,i);
        accs[i].bufferView=(int)json_num(json_find(a,"bufferView"),-1);
        accs[i].byteOffset=(long)json_num(json_find(a,"byteOffset"),0);
        accs[i].comp=(int)json_num(json_find(a,"componentType"),C_FLOAT);
        accs[i].vec=tv(json_str(json_find(a,"type"),"SCALAR"));
        accs[i].norm=json_bool(json_find(a,"normalized"),0);
        accs[i].count=(long)json_num(json_find(a,"count"),0);
        // Шаг между элементами: byteStride bufferView если задан, иначе плотная упаковка.
        int bv_i=accs[i].bufferView, elem_sz=cs(accs[i].comp)*accs[i].vec;
        accs[i].stride=(bv_i>=0&&bv_i<bv_n&&bvs[bv_i].byteStride>0)?bvs[bv_i].byteStride:elem_sz;}

    AnimGlb*out=calloc(1,sizeof* out);
    out->nodes=calloc((size_t)node_n,sizeof* out->nodes);
    if(!out||(node_n>0&&!out->nodes)){ SDL_Log("glb_anim: %s — нехватка памяти",path); free(out); free(file); return NULL; }
    out->nnode=node_n;
    for(int i=0;i<node_n;i++) out->nodes[i].parent=-1;
    // родители и дети
    for(int i=0;i<node_n;i++){const JsonNode*nd=json_array_get(nd_a,i); AnimNode*N=&out->nodes[i];
        const JsonNode*ch=json_find(nd,"children");
        snprintf(N->name,sizeof N->name,"%s",json_str(json_find(nd,"name"),""));
        if(ch&&ch->type==JSON_ARRAY){for(int k=0;k<json_array_len(ch);k++){int ci=(int)json_num(json_array_get(ch,k),-1);
            if(ci>=0&&ci<node_n&&N->nchildren<64){N->children[N->nchildren++]=ci; out->nodes[ci].parent=i;}}}
        N->mesh=(int)json_num(json_find(nd,"mesh"),-1);
        const JsonNode*mn=json_find(nd,"matrix");
        const JsonNode*tr=json_find(nd,"translation"),*qt=json_find(nd,"rotation"),*sc=json_find(nd,"scale");
        // базовые TRS-компоненты
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

    // кожи
    int nskin=sk_a&&sk_a->type==JSON_ARRAY?json_array_len(sk_a):0;
    if(nskin>0){
        out->skins=calloc((size_t)nskin,sizeof* out->skins); out->nskin=nskin;
        out->skin_of_node=calloc((size_t)node_n,sizeof* out->skin_of_node);
        if(!out->skins||(node_n>0&&!out->skin_of_node)){ out->nskin=0; }
        for(int i=0;i<node_n;i++)out->skin_of_node[i]=-1;
        for(int s=0;s<nskin;s++){const JsonNode*sk=json_array_get(sk_a,s);
            const JsonNode*jn=json_find(sk,"joints"); int jc=jn&&jn->type==JSON_ARRAY?json_array_len(jn):0;
            if(jc<0||jc>32*1024*1024) jc=0;      // потолок на число суставов
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

    // примитивы мешей (в bind pose, локальные коорд.)
    out->skin_nodes=calloc((size_t)(node_n?node_n:1),sizeof* out->skin_nodes);
    int mesh_n=ms_a&&ms_a->type==JSON_ARRAY?json_array_len(ms_a):0;
    int*mesh_pstart=calloc((size_t)(mesh_n?mesh_n:1),sizeof(int));
    int*mesh_pcount=calloc((size_t)(mesh_n?mesh_n:1),sizeof(int));
    for(int mi=0;mi<mesh_n;mi++){const JsonNode*mesh=json_array_get(ms_a,mi);
        const JsonNode*prims=json_find(mesh,"primitives");
        mesh_pstart[mi]=out->nprim;
        if(!prims||prims->type!=JSON_ARRAY){continue;}
        for(int p=0;p<json_array_len(prims);p++){const JsonNode*pr=json_array_get(prims,p);
            if((int)json_num(json_find(pr,"mode"),4)!=4)continue;
            const JsonNode*attrs=json_find(pr,"attributes");
            int ipos=(int)json_num(attrs?json_find(attrs,"POSITION"):NULL,-1);
            int inrm=(int)json_num(attrs?json_find(attrs,"NORMAL"):NULL,-1);
            // UV-набор — тот, на который ссылается baseColorTexture материала
            // (глTF позволяет до 2 развёрток: у модели лица лица/глаза могут
            // жить в TEXCOORD_1). Фолбэк — TEXCOORD_0.
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
            // Защита от OOB: акцессор обязан укладываться в bin-чанк файла.
            if(!acc_fits(ap,bvs,bv_n,(long)bin_len)){
                SDL_Log("glb_anim: %s — POSITION акцессор вне буфера, примитив пропущен",path);
                continue;
            }
            GA*an=(inrm>=0&&inrm<acc_n&&acc_fits(&accs[inrm],bvs,bv_n,(long)bin_len))?&accs[inrm]:NULL;
            GA*au=(iuv>=0&&iuv<acc_n&&acc_fits(&accs[iuv],bvs,bv_n,(long)bin_len))?&accs[iuv]:NULL;
            GA*aj=(ijo>=0&&ijo<acc_n&&acc_fits(&accs[ijo],bvs,bv_n,(long)bin_len))?&accs[ijo]:NULL;
            GA*aw=(iwe>=0&&iwe<acc_n&&acc_fits(&accs[iwe],bvs,bv_n,(long)bin_len))?&accs[iwe]:NULL;
            GA*ai=(iidx>=0&&iidx<acc_n&&acc_fits(&accs[iidx],bvs,bv_n,(long)bin_len))?&accs[iidx]:NULL;
            long nv=ap->count;
            // Разумный потолок на вершины примитива (защита от count~1e9 из JSON)
            if(nv>16*1024*1024){
                SDL_Log("glb_anim: %s — POSITION count %ld слишком велик",path,nv);
                continue;
            }
            AnimPrim*o=realloc(out->prims,(size_t)(out->nprim+1)*sizeof* out->prims);
            if(!o) goto prim_fail; out->prims=o;
            AnimPrim*P=&out->prims[out->nprim++]; memset(P,0,sizeof* P);
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
                float p[3],n[3]={0,1,0},u[2]={0,0};
                rvv(bin,pb,ap,(int)v,p);
                if(an) rvv(bin,nb,an,(int)v,n);
                if(au) rvv(bin,ub,au,(int)v,u);
                P->verts[3*v]=p[0]; P->verts[3*v+1]=p[1]; P->verts[3*v+2]=p[2];
                P->normals[3*v]=n[0]; P->normals[3*v+1]=n[1]; P->normals[3*v+2]=n[2];
                P->uvs[2*v]=u[0]; P->uvs[2*v+1]=1.0f-u[1]; // glTF: V=0 сверху; GL: t=0 снизу → флип
                if(P->bone_idx){
                    float ji[4]={0,0,0,0},we[4]={0,0,0,0};
                    if(aj) rvv(bin,jb,aj,(int)v,ji);
                    if(aw) rvv(bin,wb,aw,(int)v,we);
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
            // Сбой аллокации: освободить недостроенный примитив и пропустить.
            SDL_Log("glb_anim: %s — нехватка памяти на примитив",path);
            free(P->verts); free(P->normals); free(P->uvs);
            free(P->bone_idx); free(P->bone_w); free(P->indices);
            out->nprim--;
            continue;
        prim_ok:;
            // материал (псевдо-PBR 2004)
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
                // gltf-пропсы рендерим двусторонними: иначе односторонний салон/
                // тонкие детали пропадают (видно «сквозь», и внутрянка не видна
                // извне через стекло). Игнорируем doubleSided файла — всегда NONE.
                P->cull=MAT_CULL_NONE;
                const JsonNode*em=json_find(mt,"emissiveFactor");
                if(em&&em->type==JSON_ARRAY&&json_array_len(em)>=3){
                    P->tint[0]+=(float)json_num(json_array_get(em,0),0);
                    P->tint[1]+=(float)json_num(json_array_get(em,1),0);
                    P->tint[2]+=(float)json_num(json_array_get(em,2),0);}
                // альбедо-текстура
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
                                // REPEAT из семплера тянет 2-й тайл при UV>1 -> рваный
                                // рисунок. Для gltf-пропсов используем CLAMP (по краю).
                                P->wrap=(wS==33648||wT==33648)?MAT_WRAP_MIRROR:MAT_WRAP_CLAMP;
                                int mag=(int)json_num(json_find(sa,"magFilter"),9729);
                                P->filter=(mag==9728)?MAT_FILTER_NEAREST:MAT_FILTER_MIPMAP;}
                        }}
                }
                if(P->blend==MAT_BLEND_ALPHA&&P->img&&P->alpha>=0.95f){
                    // стекло, нарисованное непрозрачной текстурой (alpha=255):
                    // без полупрозрачных пикселей BLEND не смешивается и закрывает
                    // внутреники. Если альфа в текстуре полностью 255 — стекло.
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

    // анимации
    int nclip=an_a&&an_a->type==JSON_ARRAY?json_array_len(an_a):0;
    if(nclip>0){
        out->clips=calloc((size_t)nclip,sizeof* out->clips); out->nclip=nclip;
        for(int c=0;c<nclip;c++){const JsonNode*cl=json_array_get(an_a,c); AnimClip*C=&out->clips[c];
            const char*cname=json_str(json_find(cl,"name"),"");
            snprintf(C->name,sizeof C->name,"%s",cname[0]?cname:"unnamed");
            const JsonNode*ch_a=json_find(cl,"channels"); int nch=ch_a&&ch_a->type==JSON_ARRAY?json_array_len(ch_a):0;
            const JsonNode*sp_a=json_find(cl,"samplers");  int nsp=sp_a&&sp_a->type==JSON_ARRAY?json_array_len(sp_a):0;
            C->channels=calloc((size_t)(nch?nch:1),sizeof* C->channels); C->nch=nch;
            C->samplers=calloc((size_t)(nsp?nsp:1),sizeof* C->samplers); C->ns=nsp;
            float dur=0;
            for(int s=0;s<nsp;s++){const JsonNode*sp=json_array_get(sp_a,s); AnimSampler*Samp=&C->samplers[s];
                int inp=(int)json_num(json_find(sp,"input"),-1),outp=(int)json_num(json_find(sp,"output"),-1);
                const char*ip=json_str(json_find(sp,"interpolation"),"LINEAR");
                Samp->interp=!strcmp(ip,"STEP")?ANIM_STEP:(!strcmp(ip,"CUBICSPLINE")?ANIM_CUBIC:ANIM_LINEAR);
                if(inp>=0&&inp<acc_n&&outp>=0&&outp<acc_n&&
                   acc_fits(&accs[inp],bvs,bv_n,(long)bin_len)&&
                   acc_fits(&accs[outp],bvs,bv_n,(long)bin_len)){
                    GA*ti=&accs[inp],*vo=&accs[outp];
                    long nk=ti->count>16*1024*1024?0:ti->count;   // потолок против OOM/OOB
                    // CUBICSPLINE требует output-count >= nk*3*comp (иначе rfv читает мимо акцессора)
                    if(Samp->interp==ANIM_CUBIC&&vo->count<nk*3){ nk=0; }
                    Samp->comp=vo->vec; Samp->nkeys=nk;
                    // CUBICSPLINE хранит per key 3*comp (in-tangent, value,
                    // out-tangent); LINEAR/STEP — comp на ключ.
                    int per=(Samp->interp==ANIM_CUBIC)?3*Samp->comp:Samp->comp;
                    Samp->times=malloc((size_t)(nk?nk:1)*sizeof(float));
                    Samp->values=malloc((size_t)(nk?(size_t)nk*per:1)*sizeof(float));
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

    // skin-узлы (node со skin)
    out->nskin_nodes=0;
    for(int i=0;i<node_n;i++){int sk=(int)json_num(json_find(json_array_get(nd_a,i),"skin"),-1);
        if(sk>=0&&sk<out->nskin){out->skin_of_node[i]=sk; out->skin_nodes[out->nskin_nodes++]=i;}}

    // экземпляры: каждый узел с мешем → свои draw-примитивы (instancing допустим)
    int ninst=0;
    for(int i=0;i<node_n;i++){int m=out->nodes[i].mesh;
        if(m>=0&&m<mesh_n)ninst+=mesh_pcount[m];}
    out->instances=calloc((size_t)(ninst?ninst:1),sizeof* out->instances); out->ninst=ninst;
    for(int i=0,k=0;i<node_n;i++){int m=out->nodes[i].mesh;
        if(m<0||m>=mesh_n)continue;
        for(int p=0;p<mesh_pcount[m];p++)
            out->instances[k++]=(AnimInstance){i,mesh_pstart[m]+p,out->skin_of_node[i]};}
    free(mesh_pstart); free(mesh_pcount);

    free(bvs); free(accs);
    json_free(); free(file);
    if(out->nprim==0&&out->nnode>0){ out->err[0]=0; /* нет примитивов — ок, пусто */ }
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

// --- Быстрый peek: есть ли скин/анимация ---------------------------------------
int glb_anim_has_anim(const char*path){
    unsigned int sz=0; unsigned char*f=resources_read_file(path,&sz);
    if(!f||sz<20)return 0;
    off_t off=12; int skin=0,anim=0;
    while(off+8<=sz){
        unsigned clen,ct; memcpy(&clen,f+off,4); memcpy(&ct,f+off+4,4); off+=8;
        if(off+clen>sz) break;
        if(ct==0x4E4F534A){
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

// --- Узел (GPU) ----------------------------------------------------------------
#define MAX_BONES 64
static void anim_node_exit(const Event*,void*);   // forward (см. ниже)
typedef struct{
    GLuint vao,vbo,ebo; int index_count; GLenum index_type;
    GLuint snow_vbo;     // per-vertex "открыто небу сверху" (снег), 0 если нет
    GLuint tex; Material mat; MaterialLocs ml; float specular;
    int skin;            // индекс кожи (-1 — без скиннинга)
    int has_skin;        // рендерить с костями
    int node;            // узел, несущий этот меш (для не-скинненых: его node_world)
    int tex_owned;       // 1 — glGenTextures (из img), 0 — кеш ресурсов (notfound)
    GLuint joints_bo, weights_bo;
    int *joint_map;      // какие кости этой КОЖИ реально используются (distinct,
    int joint_n;         // позиции в её joints[]), локальные индексы вершин — 0..joint_n-1
    float *bone;         // jointMatrix этих костей для uBones (joint_n*16)
} AnimDrawItem;
typedef struct{
    const RendererContext*rc; AnimDrawItem*items; int count;
    GLuint prog; SceneUniforms su;
    GLint u_model,u_tex,u_specular,u_bones,u_hasskin,u_uvtile;
    float scale; float anim_time; float anim_speed;
    int paused; int loop;       // управление воспроизведением
    AnimGlb*data;           // растёт — данные модели (негрузятся в GPU кажд. кадр)
    float*node_world;       // глобальные матрицы узлов (вычисляются каждый кадр)
    float*skin;             // jointMatrix всех костей обрабатываемой кожи
    int skin_cap;           // вместимость skin[] (макс. joint_count по кожам)
    float*wtrs;             // рабочие TRS узлов: [i*10] = trans3+quat4+scl3 (анимир.)
    int cur_clip;           // номер активного клипа
    float radius;           // огранич. сфера геометрии в мировом масштабе
    char path[256];         // путь GLB (для инструмента anim/list)
    int enabled;            // 0 — скрыт и время не идёт, 1 — виден/играет
    float dest[3]; float move_speed; int moving;   // плавное движение к dest
} AnimParams;

// Глобальный реестр анимик-узлов (нумерация = порядок загрузки). Индекс узла
// фиксируется при регистрации и не меняется — инструмент `anim <индекс>` по нему
// обращается (узлы, удалённые через anim remove, больше не выдаются).
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

// Глобальная матрица узла из рабочих TRS (анимир. компоненты в wtrs)
static void node_global_mat(const AnimGlb*a,const float*wtrs,float*out,int idx){
    int chain[256],n=0,cur=idx;
    // Петля с защитой от циклов родителя (p==cur) — глубина выше 256 обрезается,
    // но такие иерархии патологические; остаётся консервативно.
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

// Работа с рабочим буфером: wtrs[i*10] = trans[3] quat[4] scl[3]
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

// --- Сэмплирование клавишей самплера -------------------------------------------------
// Значение comp компонент в ключе k (для CUBICSPLINE — «value» часть тройки).
static void keyval(const AnimSampler*S,int k,float*out){
    int i=(S->interp==ANIM_CUBIC)?(k*3+1)*S->comp:k*S->comp;
    memcpy(out,&S->values[i],(size_t)S->comp*sizeof(float));
}
// Индексы соседних ключей и параметр сегмента по времени t.
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
        // out-tangent узла k0, in-tangent узла k1
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
// rotation (comp=4): LINEAR = slerp, STEP, CUBICSPLINE = Hermite + норм-ка.
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

// Построить глобальные матрицы узлов с учётом анимации и jointMatrix
static void build_bones(AnimParams*p){
    AnimGlb*a=p->data; if(!a->nodes)return;
    wtrs_base(a,p->wtrs);   // рабочие TRS = базовые
    // 1) применить активный клип к рабочим TRS узлов
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
    // 2) глобальные матрицы узлов из рабочих TRS
    for(int i=0;i<a->nnode;i++){float m[16]; node_global_mat(a,p->wtrs,m,i);
        memcpy(&p->node_world[i*16],m,sizeof m); }
    // 3) jointMatrix по каждой коже, разложить по примитивам (per-primitive remap).
    //    Вершины скинятся в общем пространстве скелета, поэтому скиненным
    //    примитивам node_world НЕ нужен (он уже внутри jointMatrix); не-скиненные
    //    дети иерархии берут его из узла (см. anim_render).
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
        // Тень анимированной модели в позе текущего кадра (skinning в VS)
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
    // build_bones уже выполнен в anim_update для этого кадра — не дублируем.
    // Базовая матрица экземпляра = МИРОВАЯ матрица узла (scene.c): в ней
    // позиция, поворот, масштаб и — если узел прицеплен — матрица кости
    // родителя. Модель можно вращать и вешать на другие модели.
    // СКИНЕННЫЕ примитивы рисуются в ней: jointMatrix = world[joint]*inverseBind
    // уже в пространстве скелета. НЕ-СКИНЕННЫЕ меши (пропсы в иерархии)
    // дополнительно умножаются на node_world своего узла — иначе они лежат в
    // «сыром» масштабе файла.
    const float *inst = n->world;
    glEnable(GL_DEPTH_TEST); glUseProgram(p->prog);
    scene_uniforms_bind(&p->su,p->rc);
    if(p->u_tex>=0)glUniform1i(p->u_tex,0);
    for(int i=0;i<p->count;i++){
        AnimDrawItem*d=&p->items[i];
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

// --- Точки крепления: матрица кости (хук scene.h) --------------------------------
// Слот = индекс УЗЛА glTF. Отдаём node_world[slot] — матрицу кости в
// пространстве МОДЕЛИ (без её мировой матрицы: её scene.c умножит сам).
// Масштаб модели (p->scale) уже входит в мировую матрицу узла, а node_world
// несёт только внутреннюю иерархию файла — поэтому цепочка получается
// world(модель) * bone(модель) * local(ребёнок), как и ожидается.
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
    // Точное совпадение имени
    for (int i = 0; i < p->data->nnode; i++)
        if (strcmp(p->data->nodes[i].name, name) == 0) return i;
    // Иначе — по подстроке (в GLB кости часто зовутся "mixamorig:Hand_R")
    for (int i = 0; i < p->data->nnode; i++)
        if (p->data->nodes[i].name[0] && strstr(p->data->nodes[i].name, name))
            return i;
    return -1;
}

static AnimDrawItem*build_item(const AnimPrim*P,GLuint prog,int has_skin,int skin,int node,const float*snow){
    AnimDrawItem*d=calloc(1,sizeof* d);
    d->skin=skin; d->has_skin=has_skin; d->node=node;
    // per-primitive remap: какие кости кожи реально используются (≤ MAX_BONES).
    // Индексы костей (JOINTS_0) храним как int — компонент может быть ubyte
    // (0..255) или ushort (0..65535, Blender/Sketchfab-FBX). Приводить к байту
    // НЕЛЬЗЯ: индекс >255 затрётся, и вершины привяжутся к чужим костям.
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
        SDL_Log("glb_anim: примитив использует >=%d костей — часть вершин привязана к кости 0",MAX_BONES);
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
    else d->tex=resources_texture("img/notfound.png",4,GL_REPEAT,0);   // tex_owned=0 (кеш)
    return d;
}

SceneNode *glb_anim_make_node(const char*path,const float pos[3],float scale,const RendererContext*rc){
    // Один раз: узел AnimInstance3D при выходе из дерева (node_free_tree
    // шлёт EV_NODE_EXIT ДО free(params)) освобождает все GL-объекты и данные.
    static int s_registered=0;
    if(!s_registered){ events_listen(EV_NODE_EXIT,anim_node_exit,NULL); s_registered=1; }
    AnimGlb*a=glb_anim_parse(path);
    if(!a)return NULL;
    GLuint prog=make_program("shaders/skinned.vs","shaders/mesh.fs");
    if(!prog){glb_anim_free(a);return NULL;}
    AnimParams*p=calloc(1,sizeof* p); p->rc=rc; p->scale=scale>0?scale:1; p->prog=prog; p->data=a;
    snprintf(p->path,sizeof p->path,"%s",path); p->enabled=1;
    scene_uniforms_init(&p->su,prog);
    p->u_model=glGetUniformLocation(prog,"uModel"); p->u_tex=glGetUniformLocation(prog,"uTex");
    p->u_specular=glGetUniformLocation(prog,"uSpecular"); p->u_bones=glGetUniformLocation(prog,"uBones");
    p->u_hasskin=glGetUniformLocation(prog,"uHasSkin"); p->u_uvtile=glGetUniformLocation(prog,"uUvTile");
    p->items=calloc((size_t)(a->ninst?a->ninst:1),sizeof* p->items); p->count=a->ninst;
    p->radius=0.0f;
    // Per-vertex "открыто небу сверху" по всей модели (все примитивы файла):
    // крыша/капот получат снег, пол/сиденья под крышей — нет.
    const float**vp=malloc((size_t)(a->nprim?a->nprim:1)*sizeof*vp);
    int*vc=malloc((size_t)(a->nprim?a->nprim:1)*sizeof*vc);
    const unsigned**ip=malloc((size_t)(a->nprim?a->nprim:1)*sizeof*ip);
    int*ic=malloc((size_t)(a->nprim?a->nprim:1)*sizeof*ic);
    for(int pr=0;pr<a->nprim;pr++){
        vp[pr]=a->prims[pr].verts; vc[pr]=a->prims[pr].vcount;
        ip[pr]=a->prims[pr].indices; ic[pr]=a->prims[pr].index_count;
    }
    int snow_total=0;
    float*snow_mask=snow_mask_compute(vp,vc,ip,ic,a->nprim,&snow_total);
    free(vp); free(vc); free(ip); free(ic);
    int*prim_off=malloc((size_t)(a->nprim?a->nprim:1)*sizeof*prim_off);
    { int acc=0; for(int pr=0;pr<a->nprim;pr++){ prim_off[pr]=acc; acc+=a->prims[pr].vcount; } }
    for(int i=0;i<a->ninst;i++){
        AnimInstance*it=&a->instances[i]; AnimPrim*P=&a->prims[it->prim];
        int hs=(P->bone_idx&&it->skin>=0&&it->skin<a->nskin)?1:0;
        AnimDrawItem*bi=build_item(P,prog,hs,it->skin,it->node,
                                   snow_mask? snow_mask+prim_off[it->prim] : NULL);
        p->items[i]=*bi; free(bi);   // структура скопирована — обёртку освободить
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
    // Масштаб — в трансформ узла (scene.c соберёт мировую матрицу).
    n->scl[0]=n->scl[1]=n->scl[2]=p->scale;
    n->attach_fn=anim_attach_matrix;      // к костям можно цеплять узлы
    build_bones(p);
    node_update_transforms(n);            // мировая матрица до первого рендера
    anim_reg_add(n);
    return n;
}

// --- Освобождение ресурсов узла (вызывается по EV_NODE_EXIT) ---------------------
// node_free_tree сам освободит только структуру AnimParams; всё, что внутри
// (GPU-буферы, текстуры, программа из кеша, данные модели), — здесь.
static void anim_free_resources(SceneNode*n){
    AnimParams*p=n?n->params:NULL; if(!p)return;
    anim_reg_remove(n);
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

// --- Клипы анимации ------------------------------------------------------------
// Все функции этого раздела работают ТОЛЬКО с узлом AnimInstance3D (AnimParams).
// Для статичного GlbInstance3D params — GlbParams без поля data: чтение p->data
// было бы мусором/SEGV. Проверяем тип узла и параметры заранее.
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
    p->anim_time=0;   // старт клипа с начала
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
    build_bones(p);   // обновить позу немедленно (не дожидаясь кадра)
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
    // Масштаб теперь живёт в ТРАНСФОРМЕ узла (scene.c строит из него мировую
    // матрицу) — p->scale остаётся для радиуса отсечения и запросов API.
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
    node_free_tree(n);   // посылает EV_NODE_EXIT -> anim_free_resources (unreg+GL)
}
