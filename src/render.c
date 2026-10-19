// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franklin Cordeiro

#include "xcrt.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* generated from shaders/ by the Makefile */
extern const char shader_post_vert[], shader_win_vert[], shader_win_frag[], shader_copy_frag[],
    shader_persist_frag[], shader_blur_frag[], shader_crt_frag[];

typedef struct {
    GLuint fbo, tex;
    int    w, h;
} target;

static struct {
    GLXContext ctx;

    /* texture_from_pixmap configs: [0] depth 24, [1] depth 32 */
    GLXFBConfig pix_cfg[2];
    int         pix_fmt[2];
    bool        pix_ok[2], pix_yinv[2];
    PFNGLXBINDTEXIMAGEEXTPROC    bind;
    PFNGLXRELEASETEXIMAGEEXTPROC release;

    GLuint vao, vao_empty, vbo;
    GLuint samp_linear, samp_mip;
    GLuint p_win, p_copy, p_persist, p_blur, p_crt;

    target scene, persist[2], bloom[2];
    int    persist_idx;

    GLuint cursor_tex;
    int    cur_w, cur_h, cur_xhot, cur_yhot;

    GLXPixmap root_glx;
    GLuint    root_tex;
    int       root_w, root_h;
    bool      root_yinv;

    float   *verts;
    size_t   cap_verts;
    struct timespec t0;
    unsigned frame;
} G;

/* ------------------------------------------------------------------ */
/* shaders                                                             */

static GLuint compile(GLenum type, const char *src, const char *name)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        LOG("shader %s failed to compile:\n%s\n", name, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint program(const char *vs_src, const char *fs_src, const char *name)
{
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src, name);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src, name);
    if (!vs || !fs)
        return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        LOG("program %s failed to link:\n%s\n", name, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

#define U(prog, name) glGetUniformLocation(prog, name)

/* ------------------------------------------------------------------ */
/* GLX setup                                                           */

static bool has_ext(const char *list, const char *ext)
{
    size_t n = strlen(ext);
    for (const char *p = list; (p = strstr(p, ext)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0'))
            return true;
    return false;
}

static void pick_pixmap_config(xcrt *x, int idx, int depth)
{
    int n = 0;
    GLXFBConfig *all = glXGetFBConfigs(x->dpy, x->screen, &n);
    int best = -1, best_score = 1 << 30, best_fmt = 0;

    for (int i = 0; i < n; i++) {
        XVisualInfo *vi = glXGetVisualFromFBConfig(x->dpy, all[i]);
        if (!vi)
            continue;
        int vd = vi->depth;
        XFree(vi);
        if (vd != depth)
            continue;

        int v;
        glXGetFBConfigAttrib(x->dpy, all[i], GLX_DRAWABLE_TYPE, &v);
        if (!(v & GLX_PIXMAP_BIT))
            continue;
        glXGetFBConfigAttrib(x->dpy, all[i], GLX_BIND_TO_TEXTURE_TARGETS_EXT, &v);
        if (!(v & GLX_TEXTURE_2D_BIT_EXT))
            continue;

        int rgb = 0, rgba = 0, fmt;
        glXGetFBConfigAttrib(x->dpy, all[i], GLX_BIND_TO_TEXTURE_RGB_EXT, &rgb);
        glXGetFBConfigAttrib(x->dpy, all[i], GLX_BIND_TO_TEXTURE_RGBA_EXT, &rgba);
        int score = 0;
        if (depth == 32) {
            if (!rgba)
                continue;
            fmt = GLX_TEXTURE_FORMAT_RGBA_EXT;
        } else if (rgb) {
            fmt = GLX_TEXTURE_FORMAT_RGB_EXT;
        } else if (rgba) {
            fmt = GLX_TEXTURE_FORMAT_RGBA_EXT; /* alpha is forced to 1 in the shader */
            score += 8;
        } else {
            continue;
        }

        int db, ds, st;
        glXGetFBConfigAttrib(x->dpy, all[i], GLX_DOUBLEBUFFER, &db);
        glXGetFBConfigAttrib(x->dpy, all[i], GLX_DEPTH_SIZE, &ds);
        glXGetFBConfigAttrib(x->dpy, all[i], GLX_STENCIL_SIZE, &st);
        score += (db ? 4 : 0) + (ds ? 2 : 0) + (st ? 1 : 0);
        if (score < best_score) {
            best = i;
            best_score = score;
            best_fmt = fmt;
        }
    }

    if (best >= 0) {
        /* Mesa's software path answers GLX_DONT_CARE; like picom, treat
         * anything but False as "row 0 is the top" */
        int yinv = True;
        glXGetFBConfigAttrib(x->dpy, all[best], GLX_Y_INVERTED_EXT, &yinv);
        G.pix_cfg[idx] = all[best];
        G.pix_fmt[idx] = best_fmt;
        G.pix_yinv[idx] = yinv != False;
        G.pix_ok[idx] = true;
        DBG(x, "depth %d pixmaps: fbconfig %d, GLX_Y_INVERTED_EXT=%d\n", depth, best, yinv);
    } else {
        LOG("warning: no GLX config can bind depth-%d pixmaps; such windows will not be drawn\n", depth);
    }
    if (all)
        XFree(all);
}

void render_set_vsync(xcrt *x, bool on)
{
    const char *exts = glXQueryExtensionsString(x->dpy, x->screen);
    if (has_ext(exts, "GLX_EXT_swap_control")) {
        PFNGLXSWAPINTERVALEXTPROC f =
            (PFNGLXSWAPINTERVALEXTPROC)glXGetProcAddress((const GLubyte *)"glXSwapIntervalEXT");
        if (f) {
            f(x->dpy, x->glwin, on ? 1 : 0);
            return;
        }
    }
    if (has_ext(exts, "GLX_MESA_swap_control")) {
        PFNGLXSWAPINTERVALMESAPROC f =
            (PFNGLXSWAPINTERVALMESAPROC)glXGetProcAddress((const GLubyte *)"glXSwapIntervalMESA");
        if (f) {
            f(on ? 1 : 0);
            return;
        }
    }
    if (on && has_ext(exts, "GLX_SGI_swap_control")) {
        PFNGLXSWAPINTERVALSGIPROC f =
            (PFNGLXSWAPINTERVALSGIPROC)glXGetProcAddress((const GLubyte *)"glXSwapIntervalSGI");
        if (f)
            f(1);
    }
}

static void target_free(target *t)
{
    if (t->fbo)
        glDeleteFramebuffers(1, &t->fbo);
    if (t->tex)
        glDeleteTextures(1, &t->tex);
    memset(t, 0, sizeof(*t));
}

static bool target_make(target *t, int w, int h, GLenum ifmt)
{
    t->w = w > 0 ? w : 1;
    t->h = h > 0 ? h : 1;
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, t->w, t->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return ok;
}

void render_resize(xcrt *x)
{
    XResizeWindow(x->dpy, x->glwin, x->sw, x->sh);
    target_free(&G.scene);
    for (int i = 0; i < 2; i++) {
        target_free(&G.persist[i]);
        target_free(&G.bloom[i]);
    }
    bool ok = target_make(&G.scene, x->sw, x->sh, GL_RGBA8);
    for (int i = 0; i < 2; i++) {
        ok &= target_make(&G.persist[i], x->sw, x->sh, GL_RGBA16F);
        ok &= target_make(&G.bloom[i], x->sw / 4, x->sh / 4, GL_RGBA16F);
    }
    if (!ok)
        LOG("warning: failed to create framebuffers for %dx%d\n", x->sw, x->sh);
    DBG(x, "render targets resized to %dx%d\n", x->sw, x->sh);
}

bool render_init(xcrt *x)
{
    int ev, err;
    if (!glXQueryExtension(x->dpy, &err, &ev)) {
        LOG("GLX is not available on this display\n");
        return false;
    }
    const char *exts = glXQueryExtensionsString(x->dpy, x->screen);
    if (!has_ext(exts, "GLX_EXT_texture_from_pixmap")) {
        LOG("GLX_EXT_texture_from_pixmap is not supported by the driver\n");
        return false;
    }
    G.bind = (PFNGLXBINDTEXIMAGEEXTPROC)glXGetProcAddress((const GLubyte *)"glXBindTexImageEXT");
    G.release = (PFNGLXRELEASETEXIMAGEEXTPROC)glXGetProcAddress((const GLubyte *)"glXReleaseTexImageEXT");
    PFNGLXCREATECONTEXTATTRIBSARBPROC create_ctx = (PFNGLXCREATECONTEXTATTRIBSARBPROC)glXGetProcAddress(
        (const GLubyte *)"glXCreateContextAttribsARB");
    if (!G.bind || !G.release || !create_ctx) {
        LOG("missing GLX entry points\n");
        return false;
    }

    /* window config: double-buffered, 24-bit TrueColor */
    static const int attrs[] = {
        GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
        GLX_DOUBLEBUFFER, True, None,
    };
    int n = 0;
    GLXFBConfig *cfgs = glXChooseFBConfig(x->dpy, x->screen, attrs, &n);
    if (!cfgs || n == 0) {
        LOG("no suitable GLX framebuffer config\n");
        return false;
    }
    GLXFBConfig cfg = cfgs[0];
    for (int i = 0; i < n; i++) {
        XVisualInfo *vi = glXGetVisualFromFBConfig(x->dpy, cfgs[i]);
        bool d24 = vi && vi->depth == 24;
        if (vi)
            XFree(vi);
        if (d24) {
            cfg = cfgs[i];
            break;
        }
    }
    XFree(cfgs);

    XVisualInfo *vi = glXGetVisualFromFBConfig(x->dpy, cfg);
    XSetWindowAttributes swa = {
        .colormap = XCreateColormap(x->dpy, x->root, vi->visual, AllocNone),
        .border_pixel = 0,
        .background_pixmap = None,
    };
    x->glwin = XCreateWindow(x->dpy, x->overlay, 0, 0, x->sw, x->sh, 0, vi->depth, InputOutput, vi->visual,
                             CWColormap | CWBorderPixel | CWBackPixmap, &swa);
    XFree(vi);
    XserverRegion empty = XFixesCreateRegion(x->dpy, NULL, 0);
    XFixesSetWindowShapeRegion(x->dpy, x->glwin, ShapeInput, 0, 0, empty);
    XFixesDestroyRegion(x->dpy, empty);
    XMapWindow(x->dpy, x->glwin);

    static const int ctx_attrs[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 3,
        GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None,
    };
    trap_errors(x);
    G.ctx = create_ctx(x->dpy, cfg, NULL, True, ctx_attrs);
    if (untrap_errors(x) || !G.ctx || !glXMakeCurrent(x->dpy, x->glwin, G.ctx)) {
        LOG("could not create an OpenGL 3.3 core context\n");
        return false;
    }
    DBG(x, "GL: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    render_set_vsync(x, x->cfg.vsync);

    pick_pixmap_config(x, 0, 24);
    pick_pixmap_config(x, 1, 32);

    G.p_win = program(shader_win_vert, shader_win_frag, "win");
    G.p_copy = program(shader_post_vert, shader_copy_frag, "copy");
    G.p_persist = program(shader_post_vert, shader_persist_frag, "persist");
    G.p_blur = program(shader_post_vert, shader_blur_frag, "blur");
    G.p_crt = program(shader_post_vert, shader_crt_frag, "crt");
    if (!G.p_win || !G.p_copy || !G.p_persist || !G.p_blur || !G.p_crt)
        return false;

    glGenVertexArrays(1, &G.vao_empty);
    glGenVertexArrays(1, &G.vao);
    glGenBuffers(1, &G.vbo);
    glBindVertexArray(G.vao);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));

    glGenSamplers(1, &G.samp_linear);
    glSamplerParameteri(G.samp_linear, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(G.samp_linear, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(G.samp_linear, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(G.samp_linear, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenSamplers(1, &G.samp_mip);
    glSamplerParameteri(G.samp_mip, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(G.samp_mip, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(G.samp_mip, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(G.samp_mip, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    render_resize(x);
    clock_gettime(CLOCK_MONOTONIC, &G.t0);
    return true;
}

/* ------------------------------------------------------------------ */
/* window textures                                                     */

static void new_texture(GLuint *tex)
{
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* Wraps an X pixmap in a GLX pixmap bound to a new texture. */
static bool bind_pixmap(xcrt *x, Pixmap pm, int depth, GLXPixmap *glx, GLuint *tex, bool *yinv)
{
    int idx = depth == 32 ? 1 : depth == 24 ? 0 : -1;
    if (idx < 0 || !G.pix_ok[idx])
        return false;
    const int attrs[] = {
        GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT, GLX_TEXTURE_FORMAT_EXT, G.pix_fmt[idx], None,
    };
    trap_errors(x);
    *glx = glXCreatePixmap(x->dpy, G.pix_cfg[idx], pm, attrs);
    if (untrap_errors(x) || !*glx) {
        *glx = 0;
        return false;
    }
    new_texture(tex);
    G.bind(x->dpy, *glx, GLX_FRONT_LEFT_EXT, NULL);
    *yinv = G.pix_yinv[idx];
    return true;
}

static void unbind_pixmap(xcrt *x, GLXPixmap *glx, GLuint *tex)
{
    if (*tex) {
        glBindTexture(GL_TEXTURE_2D, *tex);
        if (*glx)
            G.release(x->dpy, *glx, GLX_FRONT_LEFT_EXT);
        glDeleteTextures(1, tex);
        *tex = 0;
    }
    if (*glx) {
        glXDestroyPixmap(x->dpy, *glx);
        *glx = 0;
    }
}

void render_release_window(xcrt *x, win *w)
{
    unbind_pixmap(x, &w->glxpixmap, &w->tex);
    if (w->pixmap) {
        XFreePixmap(x->dpy, w->pixmap);
        w->pixmap = None;
    }
}

static void update_window(xcrt *x, win *w)
{
    if (w->shape_stale) {
        w->shape_stale = false;
        if (w->rects)
            XFree(w->rects);
        w->rects = NULL;
        int n = 0, ordering;
        trap_errors(x);
        XRectangle *r = XShapeGetRectangles(x->dpy, w->id, ShapeBounding, &n, &ordering);
        if (untrap_errors(x)) {
            if (r)
                XFree(r);
            w->nrects = -1; /* fall back to the full window */
        } else {
            w->rects = r;
            w->nrects = n;
        }
    }

    if (w->pixmap_stale) {
        render_release_window(x, w);
        w->pixmap_stale = false;
        w->damaged = false;
        trap_errors(x);
        Pixmap pm = XCompositeNameWindowPixmap(x->dpy, w->id);
        if (untrap_errors(x) || !pm)
            return;
        w->pixmap = pm;
        if (!bind_pixmap(x, pm, w->depth, &w->glxpixmap, &w->tex, &w->y_inverted))
            render_release_window(x, w);
    } else if (w->damaged && w->tex) {
        /* rebinding picks up new contents on every driver */
        glBindTexture(GL_TEXTURE_2D, w->tex);
        G.release(x->dpy, w->glxpixmap, GLX_FRONT_LEFT_EXT);
        G.bind(x->dpy, w->glxpixmap, GLX_FRONT_LEFT_EXT, NULL);
        w->damaged = false;
    }
}

static void update_root_background(xcrt *x)
{
    if (!x->root_bg_stale)
        return;
    x->root_bg_stale = false;
    unbind_pixmap(x, &G.root_glx, &G.root_tex); /* the pixmap itself belongs to the wallpaper setter */
    if (!x->root_pixmap)
        return;
    Window r;
    int px, py;
    unsigned int w, h, bw, depth;
    trap_errors(x);
    Status ok = XGetGeometry(x->dpy, x->root_pixmap, &r, &px, &py, &w, &h, &bw, &depth);
    if (untrap_errors(x) || !ok)
        return;
    if (bind_pixmap(x, x->root_pixmap, depth, &G.root_glx, &G.root_tex, &G.root_yinv)) {
        G.root_w = w;
        G.root_h = h;
    }
}

void render_set_cursor(xcrt *x, XFixesCursorImage *img)
{
    size_t n = (size_t)img->width * img->height;
    uint32_t *px = malloc(n * sizeof(uint32_t));
    if (!px)
        return;
    for (size_t i = 0; i < n; i++)
        px[i] = (uint32_t)img->pixels[i]; /* unsigned long -> 32-bit premultiplied ARGB */
    if (!G.cursor_tex)
        new_texture(&G.cursor_tex);
    glBindTexture(GL_TEXTURE_2D, G.cursor_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img->width, img->height, 0, GL_BGRA, GL_UNSIGNED_BYTE, px);
    free(px);
    G.cur_w = img->width;
    G.cur_h = img->height;
    G.cur_xhot = img->xhot;
    G.cur_yhot = img->yhot;
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */

static float *reserve_verts(size_t nfloats)
{
    if (nfloats > G.cap_verts) {
        float *n = realloc(G.verts, nfloats * sizeof(float));
        if (!n)
            return NULL;
        G.verts = n;
        G.cap_verts = nfloats;
    }
    return G.verts;
}

static void push_quad(float *v, float sx0, float sy0, float sx1, float sy1,
                      float tx0, float ty0, float tx1, float ty1)
{
    const float q[24] = {
        sx0, sy0, tx0, ty0,  sx1, sy0, tx1, ty0,  sx1, sy1, tx1, ty1,
        sx0, sy0, tx0, ty0,  sx1, sy1, tx1, ty1,  sx0, sy1, tx0, ty1,
    };
    memcpy(v, q, sizeof(q));
}

static void draw_textured(GLuint tex, int tw, int th, bool flip, bool opaque, const float *v, int nverts)
{
    glUniform2f(U(G.p_win, "u_texsize"), (float)tw, (float)th);
    glUniform1i(U(G.p_win, "u_flip"), flip);
    glUniform1i(U(G.p_win, "u_opaque"), opaque);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBufferData(GL_ARRAY_BUFFER, nverts * 4 * sizeof(float), v, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, nverts);
}

static void draw_simple(GLuint tex, int x0, int y0, int w, int h, bool flip, bool opaque)
{
    float *v = reserve_verts(24);
    if (!v)
        return;
    push_quad(v, x0, y0, x0 + w, y0 + h, 0, 0, w, h);
    draw_textured(tex, w, h, flip, opaque, v, 6);
}

static void draw_window(win *w)
{
    int pw = w->w + 2 * w->bw, ph = w->h + 2 * w->bw;
    XRectangle full = { -w->bw, -w->bw, pw, ph };
    XRectangle *rs = w->nrects < 0 ? &full : w->rects;
    int n = w->nrects < 0 ? 1 : w->nrects;
    if (n == 0 || !rs)
        return;
    float *v = reserve_verts((size_t)n * 24);
    if (!v)
        return;
    int nv = 0;
    for (int i = 0; i < n; i++) {
        /* shape rects are relative to the interior; the pixmap includes the border */
        int tx0 = rs[i].x + w->bw, ty0 = rs[i].y + w->bw;
        int tx1 = tx0 + rs[i].width, ty1 = ty0 + rs[i].height;
        if (tx0 < 0) tx0 = 0;
        if (ty0 < 0) ty0 = 0;
        if (tx1 > pw) tx1 = pw;
        if (ty1 > ph) ty1 = ph;
        if (tx0 >= tx1 || ty0 >= ty1)
            continue;
        push_quad(v + nv * 4, w->x + tx0, w->y + ty0, w->x + tx1, w->y + ty1, tx0, ty0, tx1, ty1);
        nv += 6;
    }
    if (nv)
        draw_textured(w->tex, pw, ph, !w->y_inverted, w->opaque, v, nv);
}

static void bind_tex(int unit, GLuint tex, GLuint sampler)
{
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(unit, sampler);
}

static void draw_pass(const target *t)
{
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, t->w, t->h);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void set_crt_uniforms(const xcrt *x, const crt_preset *p, float time, bool has_bloom)
{
    GLuint g = G.p_crt;
    float k = x->curvature_scale;
    glUniform1i(U(g, "u_scene"), 0);
    glUniform1i(U(g, "u_bloom"), 1);
    glUniform1i(U(g, "u_has_bloom"), has_bloom);
    glUniform1f(U(g, "u_time"), time);
    glUniform1f(U(g, "u_frame"), (float)(G.frame % 100000));
    glUniform2f(U(g, "u_curvature"), p->curvature_x * k, p->curvature_y * k);
    glUniform1f(U(g, "u_corner_radius"), p->corner_radius);
    glUniform1f(U(g, "u_scanline_strength"), p->scanline_strength);
    glUniform1f(U(g, "u_scanline_size"), p->scanline_size);
    glUniform1i(U(g, "u_mask_type"), p->mask_type);
    glUniform1f(U(g, "u_mask_strength"), p->mask_strength);
    glUniform1f(U(g, "u_mask_size"), p->mask_size);
    glUniform1f(U(g, "u_softness"), p->softness);
    glUniform1f(U(g, "u_chroma"), p->chroma);
    glUniform1f(U(g, "u_bloom_strength"), p->bloom);
    glUniform1f(U(g, "u_vignette"), p->vignette);
    glUniform1f(U(g, "u_noise"), p->noise);
    glUniform1f(U(g, "u_flicker"), p->flicker);
    glUniform1f(U(g, "u_jitter"), p->jitter);
    glUniform1f(U(g, "u_roll"), p->roll);
    glUniform1f(U(g, "u_roll_speed"), p->roll_speed);
    glUniform1f(U(g, "u_brightness"), p->brightness);
    glUniform1f(U(g, "u_contrast"), p->contrast);
    glUniform1f(U(g, "u_saturation"), p->saturation);
    glUniform1f(U(g, "u_gamma"), p->gamma);
    glUniform1i(U(g, "u_monochrome"), p->monochrome);
    glUniform3f(U(g, "u_tint"), p->tint[0], p->tint[1], p->tint[2]);
}

void render_frame(xcrt *x)
{
    const crt_preset *p = current_preset(x);

    update_root_background(x);
    for (int i = 0; i < x->nwins; i++)
        if (x->wins[i].mapped && !x->wins[i].input_only)
            update_window(x, &x->wins[i]);

    /* 1. compose the desktop into the scene texture */
    glBindFramebuffer(GL_FRAMEBUFFER, G.scene.fbo);
    glViewport(0, 0, x->sw, x->sh);
    glClearColor(x->cfg.background[0], x->cfg.background[1], x->cfg.background[2], 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(0, 0, 0, 1);
    glUseProgram(G.p_win);
    glUniform2f(U(G.p_win, "u_screen"), (float)x->sw, (float)x->sh);
    glUniform1i(U(G.p_win, "u_tex"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, 0);
    glBindVertexArray(G.vao);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    if (G.root_tex)
        draw_simple(G.root_tex, 0, 0, G.root_w, G.root_h, !G.root_yinv, true);
    for (int i = 0; i < x->nwins; i++) {
        win *w = &x->wins[i];
        if (w->mapped && w->tex)
            draw_window(w);
    }
    if (x->cursor_hidden && G.cursor_tex)
        draw_simple(G.cursor_tex, x->ptr_x - G.cur_xhot, x->ptr_y - G.cur_yhot, G.cur_w, G.cur_h, false, false);
    glDisable(GL_BLEND);

    /* 2. post-processing passes */
    glBindVertexArray(G.vao_empty);
    GLuint src = G.scene.tex;
    bool fx = x->enabled;

    if (fx && p->persistence > 0) {
        target *dst = &G.persist[G.persist_idx], *prev = &G.persist[G.persist_idx ^ 1];
        glUseProgram(G.p_persist);
        glUniform1i(U(G.p_persist, "u_cur"), 0);
        glUniform1i(U(G.p_persist, "u_prev"), 1);
        glUniform1f(U(G.p_persist, "u_decay"), fminf(p->persistence, 0.98f));
        bind_tex(0, src, G.samp_linear);
        bind_tex(1, prev->tex, G.samp_linear);
        draw_pass(dst);
        src = dst->tex;
        G.persist_idx ^= 1;
    }

    bool has_bloom = fx && p->bloom > 0;
    if (has_bloom) {
        glBindTexture(GL_TEXTURE_2D, src);
        glGenerateMipmap(GL_TEXTURE_2D);
        glUseProgram(G.p_blur);
        glUniform1i(U(G.p_blur, "u_src"), 0);
        float r = p->bloom_radius > 0 ? p->bloom_radius : 1.0f;
        bind_tex(0, src, G.samp_mip);
        glUniform1f(U(G.p_blur, "u_lod"), 2.0f);
        glUniform2f(U(G.p_blur, "u_dir"), r / G.bloom[0].w, 0);
        draw_pass(&G.bloom[0]);
        bind_tex(0, G.bloom[0].tex, G.samp_linear);
        glUniform1f(U(G.p_blur, "u_lod"), 0.0f);
        glUniform2f(U(G.p_blur, "u_dir"), 0, r / G.bloom[0].h);
        draw_pass(&G.bloom[1]);
    }

    /* 3. CRT (or passthrough) per monitor onto the screen */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, x->sw, x->sh);
    glClear(GL_COLOR_BUFFER_BIT);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    float t = (float)fmod((now.tv_sec - G.t0.tv_sec) + (now.tv_nsec - G.t0.tv_nsec) * 1e-9, 3600.0);

    GLuint prog = fx ? G.p_crt : G.p_copy;
    glUseProgram(prog);
    bind_tex(0, src, G.samp_linear);
    bind_tex(1, has_bloom ? G.bloom[1].tex : src, G.samp_linear);
    if (fx)
        set_crt_uniforms(x, p, t, has_bloom);
    else
        glUniform1i(U(prog, "u_scene"), 0);

    for (int i = 0; i < x->nmons; i++) {
        rect m = x->mons[i];
        int gy = x->sh - m.y - m.h; /* GL origin is bottom-left */
        glViewport(m.x, gy, m.w, m.h);
        glUniform4f(U(prog, "u_mon"), (float)m.x / x->sw, (float)gy / x->sh, (float)m.w / x->sw,
                    (float)m.h / x->sh);
        if (fx) {
            glUniform2f(U(prog, "u_mon_px"), (float)m.w, (float)m.h);
            glUniform2f(U(prog, "u_origin_px"), (float)m.x, (float)gy);
        }
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    glXSwapBuffers(x->dpy, x->glwin);
    G.frame++;
}

void render_cleanup(xcrt *x)
{
    if (!G.ctx)
        return;
    unbind_pixmap(x, &G.root_glx, &G.root_tex);
    if (G.cursor_tex)
        glDeleteTextures(1, &G.cursor_tex);
    target_free(&G.scene);
    for (int i = 0; i < 2; i++) {
        target_free(&G.persist[i]);
        target_free(&G.bloom[i]);
    }
    glDeleteSamplers(1, &G.samp_linear);
    glDeleteSamplers(1, &G.samp_mip);
    glDeleteBuffers(1, &G.vbo);
    glDeleteVertexArrays(1, &G.vao);
    glDeleteVertexArrays(1, &G.vao_empty);
    GLuint progs[] = { G.p_win, G.p_copy, G.p_persist, G.p_blur, G.p_crt };
    for (int i = 0; i < 5; i++)
        if (progs[i])
            glDeleteProgram(progs[i]);
    free(G.verts);
    glXMakeCurrent(x->dpy, None, NULL);
    glXDestroyContext(x->dpy, G.ctx);
    G.ctx = NULL;
    if (x->glwin)
        XDestroyWindow(x->dpy, x->glwin);
    x->glwin = None;
}
