// ============================================================================
//  material.c — материал поверхности: состояние GL и uniform'ы (см. material.h)
// ============================================================================
#include "material.h"

#include <string.h>

void material_defaults(Material *m)
{
    memset(m, 0, sizeof *m);
    m->tint[0] = m->tint[1] = m->tint[2] = 1.0f;
    m->alpha = 1.0f;
    m->blend = MAT_BLEND_ALPHATEST;
    m->cull = MAT_CULL_NONE;
    m->depth_write = 1;
    m->wrap = MAT_WRAP_CLAMP;
    m->filter = MAT_FILTER_LINEAR;
    m->tile[0] = m->tile[1] = 1.0f;
    m->offset[0] = m->offset[1] = 0.0f;
    m->scroll[0] = m->scroll[1] = 0.0f;
}

void material_locs_init(MaterialLocs *ml, GLuint prog)
{
    memset(ml, 0, sizeof *ml);
    ml->u_tint    = glGetUniformLocation(prog, "uTint");
    ml->u_blend   = glGetUniformLocation(prog, "uBlend");
    ml->u_alpha   = glGetUniformLocation(prog, "uAlpha");
    ml->u_uvtile  = glGetUniformLocation(prog, "uUvTile");
    ml->u_uvoffset = glGetUniformLocation(prog, "uUvOffset");
    ml->u_uvscroll = glGetUniformLocation(prog, "uUvScroll");
    ml->u_time    = glGetUniformLocation(prog, "uTime");
}

// Применяет материал: состояние GL (блендинг/отсечение/глубина) и uniform'ы
// (tint/alpha/blend/UV-анимация). Дефолты материала заполняются заранее
// (material_defaults), поэтому uniform'ы применяются ВСЕГДА — значения от
// предыдущего инстанса не «протекают».
void material_bind(const Material *m, const MaterialLocs *ml, float time)
{
    // --- Состояние GL ---
    if (m->set) {
        if (m->blend == MAT_BLEND_ALPHA) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        } else if (m->blend == MAT_BLEND_ADDITIVE) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        } else {
            glDisable(GL_BLEND);
        }
        if (m->cull == MAT_CULL_NONE) {
            glDisable(GL_CULL_FACE);
        } else {
            glEnable(GL_CULL_FACE);
            glCullFace(m->cull == MAT_CULL_BACK ? GL_BACK : GL_FRONT);
        }
        glDepthMask(m->depth_write ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_TRUE);
    }
    // --- Uniform'ы (необязательные = -1) ---
    if (ml->u_tint >= 0)
        glUniform3f(ml->u_tint, m->tint[0], m->tint[1], m->tint[2]);
    if (ml->u_blend >= 0) glUniform1i(ml->u_blend, m->blend);
    if (ml->u_alpha >= 0) glUniform1f(ml->u_alpha, m->alpha);
    if (ml->u_uvtile >= 0)
        glUniform2f(ml->u_uvtile, m->tile[0], m->tile[1]);
    if (ml->u_uvoffset >= 0)
        glUniform2f(ml->u_uvoffset, m->offset[0], m->offset[1]);
    if (ml->u_uvscroll >= 0)
        glUniform2f(ml->u_uvscroll, m->scroll[0], m->scroll[1]);
    if (ml->u_time >= 0) glUniform1f(ml->u_time, time);
}
