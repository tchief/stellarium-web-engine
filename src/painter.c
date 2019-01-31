/* Stellarium Web Engine - Copyright (c) 2018 - Noctua Software Ltd
 *
 * This program is licensed under the terms of the GNU AGPL v3, or
 * alternatively under a commercial licence.
 *
 * The terms of the AGPL v3 license can be found in the main directory of this
 * repository.
 */

#include "swe.h"
#include <float.h>

static bool g_debug = false;

#define REND(rend, f, ...) do { \
        if ((rend)->f) (rend)->f((rend), ##__VA_ARGS__); \
    } while (0)


/*
 * Function: compute_viewport_cap
 * Compute the viewport cap (in given frame).
 */
static void compute_viewport_cap(const painter_t *painter, int frame,
                                 double cap[4])
{
    int i;
    double p[3];
    const double w = painter->proj->window_size[0];
    const double h = painter->proj->window_size[1];
    double max_sep = 0;

    painter_unproject(painter, frame, VEC(w / 2, h / 2), cap);
    // Compute max separation from all corners.
    for (i = 0; i < 4; i++) {
        painter_unproject(painter, frame, VEC(w * (i % 2), h * (i / 2)), p);
        max_sep = max(max_sep, eraSepp(cap, p));
    }
    cap[3] = cos(max_sep);
}

static void compute_sky_cap(const observer_t *obs, int frame, double cap[4])
{
    double p[3] = {0, 0, 1};
    convert_frame(obs, FRAME_OBSERVED, frame, true, p, cap);
    cap[3] = cos(91.0 * M_PI / 180);
}

void painter_update_caps(const painter_t *painter)
{
    int i;
    for (i = 0; i < FRAMES_NB ; ++i) {
        compute_viewport_cap(painter, i, painter->viewport_caps[i]);
        compute_sky_cap(painter->obs, i, painter->sky_caps[i]);
    }
}

int paint_prepare(const painter_t *painter, double win_w, double win_h,
                  double scale)
{
    PROFILE(paint_prepare, 0);
    areas_clear_all(core->areas);
    REND(painter->rend, prepare, win_w, win_h, scale);
    return 0;
}

int paint_finish(const painter_t *painter)
{
    PROFILE(paint_finish, 0);
    REND(painter->rend, finish);
    return 0;
}

int paint_flush(const painter_t *painter)
{
    PROFILE(paint_flush, 0);
    REND(painter->rend, flush);
    return 0;
}

int paint_2d_points(const painter_t *painter, int n, const point_t *points)
{
    PROFILE(paint_2d_points, PROFILE_AGGREGATE);
    REND(painter->rend, points_2d, painter, n, points);
    return 0;
}

static int paint_quad_visitor(int step, qtree_node_t *node,
                              const double uv[4][2], const double pos[4][4],
                              const painter_t *painter,
                              void *user,
                              int s[2])
{
    texture_t *tex = USER_GET(user, 0);
    texture_t *normalmap_tex = USER_GET(user, 1);
    projection_t *tex_proj = USER_GET(user, 2);
    int frame = *(int*)USER_GET(user, 3);
    int grid_size = *(int*)USER_GET(user, 4);

    if (step == 0) {
        if ((1 << node->level) > grid_size) return 0;
        return 2;
    }
    if (step == 1) return 2;
    REND(painter->rend, quad, painter, frame,
                        tex, normalmap_tex, uv,
                        grid_size >> node->level,
                        tex_proj);

    if (g_debug) {
        REND(painter->rend, quad_wireframe, painter, frame, uv,
             grid_size >> node->level, tex_proj);
    }
    return 0;
}

int paint_quad(const painter_t *painter,
               int frame,
               texture_t *tex,
               texture_t *normalmap_tex,
               const double uv[4][2],
               const projection_t *tex_proj,
               int grid_size)
{
    PROFILE(paint_quad, PROFILE_AGGREGATE);
    // The OBSERVED frame (azalt) is left handed, so if we didn't specify
    // the uv values, we use inverted uv so that things work by default.
    const double UV1[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    const double UV2[4][2] = {{1, 0}, {0, 0}, {1, 1}, {0, 1}};
    qtree_node_t nodes[128];
    if (tex && !texture_load(tex, NULL)) return 0;
    if (painter->color[3] == 0.0) return 0;
    if (!uv) uv = frame == FRAME_OBSERVED ? UV2 : UV1;
    traverse_surface(nodes, ARRAY_SIZE(nodes), uv, tex_proj,
                     painter, frame, 0,
                     USER_PASS(tex, normalmap_tex, tex_proj,
                               &frame, &grid_size),
                     paint_quad_visitor);
    return 0;
}

// Estimate the number of pixels covered by a quad.
double paint_quad_area(const painter_t *painter,
                       const double uv[4][2],
                       const projection_t *proj)
{
    const double DEFAULT_UV[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    double p[4][4] = {};
    double u[2], v[2];
    int i;
    uv = uv ?: DEFAULT_UV;
    double view_mat[4][4];

    // Clipping space coordinates.
    for (i = 0; i < 4; i++) {
        vec4_set(p[i], uv[i][0], uv[i][1], 0, 1);
        project(proj, PROJ_BACKWARD, 4, p[i], p[i]);
        mat4_mul_vec3(view_mat, p[i], p[i]);
        project(painter->proj, 0, 4, p[i], p[i]);
    }
    if (is_clipped(4, p)) return NAN;
    // Screen coordinates.
    for (i = 0; i < 4; i++) {
        vec2_mul(1.0 / p[i][3], p[i], p[i]);
        p[i][0] = (p[i][0] + 1.0) * painter->fb_size[0] / 2;
        p[i][1] = (p[i][1] + 1.0) * painter->fb_size[1] / 2;
    }
    // Return the cross product.
    vec2_sub(p[1], p[0], u);
    vec2_sub(p[2], p[0], v);
    return fabs(u[0] * v[1] - u[1] * v[0]);
}

int paint_text_bounds(const painter_t *painter, const char *text,
                      const double pos[2], int align, double size,
                      double bounds[4])
{
    REND(painter->rend, text, text, pos, align, size, NULL, 0, painter->font,
         bounds);
    return 0;
}

int paint_text(const painter_t *painter, const char *text,
               const double pos[2], int align, double size,
               const double color[4], double angle)
{
    REND(painter->rend, text, text, pos, align, size, color, angle,
         painter->font, NULL);
    return 0;
}

int paint_texture(const painter_t *painter,
                  texture_t *tex,
                  const double uv[4][2],
                  const double pos[2],
                  double size,
                  const double color[4],
                  double angle)
{
    double c[4];
    const double white[4] = {1, 1, 1, 1};
    const double uv_full[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    if (!texture_load(tex, NULL)) return 0;
    if (!color) color = white;
    if (!uv) uv = uv_full;
    vec4_emul(painter->color, color, c);
    REND(painter->rend, texture, tex, uv, pos, size, c, angle);
    return 0;
}

static int paint_line(const painter_t *painter,
                      int frame,
                      double line[2][4], const projection_t *line_proj,
                      int split,
                      int intersect_discontinuity_mode)
{
    int mode = intersect_discontinuity_mode;
    int r, i;
    double view_pos[2][4];

    if (mode > 0 && painter->proj->intersect_discontinuity) {
        for (i = 0; i < 2; i++) {
            if (line_proj)
                project(line_proj, PROJ_BACKWARD, 4, line[i], view_pos[i]);
            else
                memcpy(view_pos[i], line[i], sizeof(view_pos[i]));
        }
        r = painter->proj->intersect_discontinuity(
                            painter->proj, view_pos[0], view_pos[1]);
        if (r & PROJ_INTERSECT_DISCONTINUITY) {
            return r;
        }
    }

    REND(painter->rend, line, painter, frame, line, split, line_proj);
    return 0;
}

int paint_lines(const painter_t *painter,
                int frame,
                int nb, double (*lines)[4],
                const projection_t *line_proj,
                int split,
                int intersect_discontinuity_mode)
{
    int i, ret = 0;
    assert(nb % 2 == 0);
    // XXX: we should check for discontinutiy before we cann paint_line.
    // So that we don't abort in the middle of the rendering.
    for (i = 0; i < nb; i += 2)
        ret |= paint_line(painter, frame, lines ? (void*)lines[i] : NULL,
                          line_proj, split, intersect_discontinuity_mode);
    return ret;
}

void paint_debug(bool value)
{
    g_debug = value;
}

bool painter_is_cap_clipped_fast(const painter_t *painter, int frame,
                                 const double cap[4])
{
    if (!cap_intersects_cap(painter->viewport_caps[frame], cap))
        return true;

    // Skip if below horizon.
    if (painter->flags & PAINTER_HIDE_BELOW_HORIZON &&
            !cap_intersects_cap(painter->sky_caps[frame], cap))
        return true;

    return false;
}

bool painter_is_point_clipped_fast(const painter_t *painter, int frame,
                                   const double pos[3], bool is_normalized)
{
    double v[3];
    vec3_copy(pos, v);
    if (!is_normalized)
        vec3_normalize(v, v);
    if (!cap_contains_vec3(painter->viewport_caps[frame], v))
        return true;
    if ((painter->flags & PAINTER_HIDE_BELOW_HORIZON) &&
         !cap_contains_vec3(painter->sky_caps[frame], v))
        return true;
    return false;
}

bool painter_is_tile_clipped(const painter_t *painter, int frame,
                             int order, int pix, bool outside)
{
    double healpix[4][3];
    double quad[4][4];
    double p[4][4];
    int i;

    // At order zero, the tiles are too big and it can give false positive,
    // so in that case is check the four tiles of level one.
    if (outside && order < 1) {
        for (i = 0; i < 4; i++) {
            if (!painter_is_tile_clipped(
                        painter, frame, order + 1, pix * 4 + i, outside))
                return false;
        }
        return true;
    }

    healpix_get_boundaries(1 << order, pix, healpix);
    for (i = 0; i < 4; i++) {
        vec3_copy(healpix[i], quad[i]);
        quad[i][3] = 1.0;
        mat4_mul_vec4(*painter->transform, quad[i], quad[i]);
        convert_framev4(painter->obs, frame, FRAME_VIEW, quad[i], quad[i]);
        project(painter->proj, 0, 4, quad[i], p[i]);
    }
    if (is_clipped(4, p)) return true;

    /*
     * Special case: if all the points are behind us and none are visible
     * on screen, we assume the tile is clipped.  This fix the problem
     * that the stereographic projection as defined at the moment doesn't
     * make the clipping test very accurate.
     */
    if (outside) {
        for (i = 0; i < 4; i++) {
            if (p[i][0] >= -p[i][3] && p[i][0] <= +p[i][3] &&
                p[i][1] >= -p[i][3] && p[i][1] <= +p[i][3] &&
                p[i][2] >= -p[i][3] && p[i][2] <= +p[i][3]) return false;
            if (quad[i][2] < 0) return false;
        }
        return true;
    }
    return false;
}

/* Draw the contour lines of a shape.
 * The shape is defined by the parametric function `proj`, that maps:
 * (u, v) -> (x, y, z) for u and v in the range [0, 1].
 *
 * borders_mask is a 4 bits mask to decide what side of the uv rect has to be
 * rendered (should be all set for a rect).
 */
int paint_quad_contour(const painter_t *painter, int frame,
                       const projection_t *proj,
                       int split, int borders_mask)
{
    const double lines[4][2][4] = {
        {{0, 0}, {1, 0}},
        {{1, 0}, {1, 1}},
        {{1, 1}, {0, 1}},
        {{0, 1}, {0, 0}}
    };
    int i;
    for (i = 0; i < 4; i++) {
        if (!((1 << i) & borders_mask)) continue;
        paint_line(painter, frame, lines[i], proj, split, 0);
    }
    return 0;
}

/*
 * Function: paint_tile_contour
 * Draw the contour lines of an healpix tile.
 *
 * This is mostly useful for debugging.
 *
 * Parameters:
 *   painter    - A painter.
 *   frame      - One the <FRAME> enum value.
 *   order      - Healpix order.
 *   pix        - Healpix pix.
 *   split      - Number or times we split the lines.
 */
int paint_tile_contour(const painter_t *painter, int frame,
                       int order, int pix, int split)
{
    projection_t proj;
    projection_init_healpix(&proj, 1 << order, pix, false, false);
    return paint_quad_contour(painter, frame, &proj, split, 15);
}

static void orbit_project(const projection_t *proj, int flags,
                          const double *v, double *out)
{
    const double *o = proj->user;
    double pos[4];
    double period = 2 * M_PI / o[5]; // Period in day.
    double mjd = o[0] + period * v[0];
    orbit_compute_pv(0, mjd, pos, NULL,
                     o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7], 0.0, 0.0);
    vec3_copy(pos, out);
    out[3] = 1.0; // AU.
}

/*
 * Function: paint_orbit
 * Draw an orbit from it's elements.
 *
 * Parameters:
 *   painter    - The painter.
 *   frame      - Need to be FRAME_ICRF.
 *   k_jd       - Orbit epoch date (MJD).
 *   k_in       - Inclination (rad).
 *   k_om       - Longitude of the Ascending Node (rad).
 *   k_w        - Argument of Perihelion (rad).
 *   k_a        - Mean distance (Semi major axis).
 *   k_n        - Daily motion (rad/day).
 *   k_ec       - Eccentricity.
 *   k_ma       - Mean Anomaly (rad).
 */
int paint_orbit(const painter_t *painter, int frame,
                double k_jd,      // date (MJD).
                double k_in,      // inclination (rad).
                double k_om,      // Longitude of the Ascending Node (rad).
                double k_w,       // Argument of Perihelion (rad).
                double k_a,       // Mean distance (Semi major axis).
                double k_n,       // Daily motion (rad/day).
                double k_ec,      // Eccentricity.
                double k_ma)      // Mean Anomaly (rad).
{
    const double orbit[8] = {k_jd, k_in, k_om, k_w, k_a, k_n, k_ec, k_ma};
    projection_t orbit_proj = {
        .backward   = orbit_project,
        .user       = (void*)orbit,
    };
    double line[2][4] = {{0}, {1}};
    // We only support ICRS for the moment to make things simpler.
    assert(frame == FRAME_ICRF);
    paint_line(painter, frame, line, &orbit_proj, 128, 1);
    return 0;
}

/*
 * Function: paint_2d_ellipse
 * Paint an ellipse in 2d.
 *
 * Parameters:
 *   painter    - The painter.
 *   transf     - Transformation from unit into window space that defines
 *                the shape position, orientation and scale.
 *   pos        - The ellipse position in window space.
 *   size       - The ellipse size in window space.
 *   dashes     - Size of the dashes (0 for a plain line).
 *   label_pos  - Output the position that could be used for a label.  Can
 *                be NULL.
 */
int paint_2d_ellipse(const painter_t *painter_,
                     const double transf[3][3], double dashes,
                     const double pos[2],
                     const double size[2],
                     double label_pos[2])
{
    double a2, b2, perimeter, p[3], s[2], a, m[3][3], angle;
    painter_t painter = *painter_;

    // Apply the pos, size and angle.
    mat3_set_identity(m);
    if (pos) mat3_itranslate(m, pos[0], pos[1]);
    if (size) mat3_iscale(m, size[0], size[1], 1);
    if (transf) mat3_mul(m, transf, m);

    a2 = vec2_norm2(m[0]);
    b2 = vec2_norm2(m[1]);

    // Estimate the number of dashes.
    painter.lines_stripes = 0;
    if (dashes) {
        perimeter = 2 * M_PI * sqrt((a2 + b2) / 2);
        painter.lines_stripes = perimeter / dashes;
    }

    vec2_copy(m[2], p);
    s[0] = sqrt(a2);
    s[1] = sqrt(b2);
    angle = atan2(m[0][1], m[0][0]);
    REND(painter.rend, ellipse_2d, &painter, p, s, angle);

    if (label_pos) {
        label_pos[1] = DBL_MAX;
        for (a = 0; a < 2 * M_PI / 2; a += 2 * M_PI / 16) {
            vec3_set(p, cos(a), sin(a), 1);
            mat3_mul_vec3(m, p, p);
            if (p[1] < label_pos[1]) vec2_copy(p, label_pos);
        }
    }
    return 0;
}

/*
 * Function: paint_2d_rect
 * Paint a rect in 2d.
 *
 * Parameters:
 *   painter    - The painter.
 *   transf     - Transformation from unit into window space that defines
 *                the shape position, orientation and scale.
 *   pos        - Top left position in window space.  If set to NULL, center
 *                the rect at the origin.
 *   size       - Size in window space.  Default value to a rect of size 1.
 */
int paint_2d_rect(const painter_t *painter, const double transf[3][3],
                  const double pos[2], const double size[2])
{
    double p[3], s[2], angle, m[3][3];

    mat3_set_identity(m);
    if (pos) mat3_itranslate(m, pos[0] + size[0] / 2, pos[1] + size[1] / 2);
    if (size) mat3_iscale(m, size[0] / 2, size[1] / 2, 1);
    if (transf) mat3_mul(m, transf, m);

    vec2_copy(m[2], p);
    s[0] = vec2_norm(m[0]);
    s[1] = vec2_norm(m[1]);
    angle = atan2(m[0][1], m[0][0]);
    REND(painter->rend, rect_2d, painter, p, s, angle);
    return 0;
}

/*
 * Function: paint_2d_line
 * Paint a line in 2d.
 *
 * Parameters:
 *   painter    - The painter.
 *   transf     - Transformation from unit into window space that defines
 *                the shape position, orientation and scale.
 *   p1         - First pos, in unit coordinates (-1 to 1).
 *   p2         - Second pos, in unit coordinates (-1 to 1).
 */
int paint_2d_line(const painter_t *painter, const double transf[3][3],
                  const double p1[2], const double p2[2])
{
    double p1_win[3] = {p1[0], p1[1], 1};
    double p2_win[3] = {p2[0], p2[1], 1};
    mat3_mul_vec3(transf, p1_win, p1_win);
    mat3_mul_vec3(transf, p2_win, p2_win);
    REND(painter->rend, line_2d, painter, p1_win, p2_win);
    return 0;
}


void painter_project_ellipse(const painter_t *painter, int frame,
        float ra, float de, float angle, float size_x, float size_y,
        double win_pos[2], double win_size[2], double *win_angle)
{
    double p[4], c[2], a[2], b[2], mat[3][3];

    assert(!isnan(ra));
    assert(!isnan(de));
    assert(!isnan(size_x));

    if (isnan(size_y)) {
        size_y = size_x;
    }

    // 1. Center.
    vec4_set(p, 1, 0, 0, 0);
    mat3_set_identity(mat);
    mat3_rz(ra, mat, mat);
    mat3_ry(-de, mat, mat);
    mat3_mul_vec3(mat, p, p);
    convert_frame(painter->obs, frame, FRAME_VIEW, true, p, p);
    project(painter->proj, PROJ_TO_WINDOW_SPACE, 2, p, c);

    // Point ellipse.
    if (size_x == 0) {
        vec2_copy(c, win_pos);
        vec2_set(win_size, 0, 0);
        *win_angle = 0;
        return;
    }

    // 2. Semi major.
    vec4_set(p, 1, 0, 0, 0);
    mat3_set_identity(mat);
    mat3_rz(ra, mat, mat);
    mat3_ry(-de, mat, mat);
    if (!isnan(angle)) mat3_rx(-angle, mat, mat);
    mat3_iscale(mat, 1.0, size_y / size_x, 1.0);
    mat3_rz(size_x / 2.0, mat, mat);
    mat3_mul_vec3(mat, p, p);
    vec3_normalize(p, p);
    convert_frame(painter->obs, frame, FRAME_VIEW, true, p, p);
    project(painter->proj, PROJ_TO_WINDOW_SPACE, 2, p, a);

    // 3. Semi minor.
    vec4_set(p, 1, 0, 0, 0);
    mat3_set_identity(mat);
    mat3_rz(ra, mat, mat);
    mat3_ry(-de, mat, mat);
    if (!isnan(angle)) mat3_rx(-angle, mat, mat);
    mat3_iscale(mat, 1.0, size_y / size_x, 1.0);
    mat3_rx(-M_PI / 2, mat, mat);
    mat3_rz(size_x / 2.0, mat, mat);
    mat3_mul_vec3(mat, p, p);
    vec3_normalize(p, p);
    convert_frame(painter->obs, frame, FRAME_VIEW, true, p, p);
    project(painter->proj, PROJ_TO_WINDOW_SPACE, 2, p, b);

    vec2_copy(c, win_pos);
    vec2_sub(a, c, a);
    vec2_sub(b, c, b);
    *win_angle = isnan(angle) ? 0 : atan2(a[1], a[0]);
    win_size[0] = 2 * vec2_norm(a);
    win_size[1] = 2 * vec2_norm(b);
}

bool painter_project(const painter_t *painter, int frame,
                     const double pos[3], bool at_inf, bool clip_first,
                     double win_pos[2]) {
    double v[3];
    if (clip_first) {
        if (painter_is_point_clipped_fast(painter, frame, pos, at_inf))
            return false;
    }
    convert_frame(painter->obs, frame, FRAME_VIEW, at_inf, pos, v);
    return project(painter->proj, (at_inf ? PROJ_ALREADY_NORMALIZED : 0) |
                   PROJ_TO_WINDOW_SPACE, 2, v, win_pos);
}

bool painter_unproject(const painter_t *painter, int frame,
                     const double win_pos[2], double pos[3]) {
    double p[4];
    // Win to NDC.
    p[0] = win_pos[0] / painter->proj->window_size[0] * 2 - 1;
    p[1] = 1 - win_pos[1] / painter->proj->window_size[1] * 2;
    // NDC to view.
    project(painter->proj, PROJ_BACKWARD, 4, p, p);
    convert_frame(painter->obs, FRAME_VIEW, frame, true, p, pos);
    return true;
}
