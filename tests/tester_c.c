/*
 * tester_c.c -- loader tests for tiny_obj_c (pure C11, acutest).
 * Run from the tests/ directory so that "../models/..." paths resolve.
 * Built with TOBJ_ENABLE_FILE_IO + TOBJ_ENABLE_MMAP under ASan+UBSan.
 */
#include "acutest.h"
#include "tiny_obj_c.h"

#include <stdio.h>
#include <string.h>

/* ---- helpers ----------------------------------------------------------- */

static tobj_result load_file_f(const char *path, tobj_scene_f *sc) {
  tobj_load_config cfg = tobj_default_config();
  return tobj_load_obj_from_file_f(sc, path, &cfg, NULL);
}

/* sum of per-face vertex counts must equal the index count */
static int mesh_consistent_f(const tobj_scene_f *sc) {
  for (size_t s = 0; s < sc->num_shapes; s++) {
    const tobj_mesh_f *m = &sc->shapes[s].mesh;
    size_t sum = 0;
    for (size_t f = 0; f < m->num_faces; f++) sum += m->num_face_vertices[f];
    if (sum != m->num_indices) return 0;
    /* every index that references a vertex must be in range */
    size_t numv = sc->attrib.vertices.count / 3;
    for (size_t i = 0; i < m->num_indices; i++) {
      int vi = m->indices[i].vertex_index;
      if (vi >= 0 && (size_t)vi >= numv) return 0;
    }
  }
  return 1;
}

static const char *load_string_f(const char *obj, tobj_scene_f *sc,
                                  bool triangulate) {
  tobj_load_config cfg = tobj_default_config();
  cfg.triangulate = triangulate;
  tobj_result r =
      tobj_load_obj_from_memory_f(sc, (const uint8_t *)obj, strlen(obj), &cfg,
                                  NULL);
  return tobj_result_string(r);
}

/* Load with index validation explicitly on or off, aggregating diagnostics. */
static tobj_result load_strict_f(const char *obj, bool strict, tobj_scene_f *sc,
                                 tobj_diag *diag) {
  tobj_load_config cfg = tobj_default_config();
  cfg.strict_index_validation = strict;
  return tobj_load_obj_from_memory_f(sc, (const uint8_t *)obj, strlen(obj),
                                     &cfg, diag);
}

typedef struct mem_io {
  const uint8_t *data;
  size_t len;
  size_t pos;
  size_t chunk;
  int closed;
} mem_io;

static tobj_result mem_io_read(void *ud, uint8_t *dst, size_t dst_size,
                               size_t *bytes_read) {
  mem_io *m = (mem_io *)ud;
  size_t left = m->len - m->pos;
  size_t n = left < dst_size ? left : dst_size;
  if (m->chunk && n > m->chunk) n = m->chunk;
  if (n) memcpy(dst, m->data + m->pos, n);
  m->pos += n;
  *bytes_read = n;
  return TOBJ_OK;
}

static void mem_io_close(void *ud) { ((mem_io *)ud)->closed = 1; }

static tobj_result io_overreport_read(void *ud, uint8_t *dst, size_t dst_size,
                                      size_t *bytes_read) {
  (void)ud;
  if (dst_size) dst[0] = 'x';
  *bytes_read = dst_size + 1;
  return TOBJ_OK;
}

static tobj_result io_error_read(void *ud, uint8_t *dst, size_t dst_size,
                                 size_t *bytes_read) {
  (void)ud;
  (void)dst;
  (void)dst_size;
  *bytes_read = 0;
  return TOBJ_ERR_IO;
}

/* ---- corpus ------------------------------------------------------------ */

/* Only files committed to the repo (CI checks out a clean tree; sandbox files
 * are local scratch files and are intentionally not used here). The
 * pathological-geometry triangulation cases are covered directly by
 * tests/tess_tester.c. */
static const char *kCorpus[] = {
    "../models/cube.obj",
    "../models/cornell_box.obj",
    "../models/catmark_torus_creases0.obj",
    "../models/smoothing-group-two-squares.obj",
    "../models/issue-162-smoothing-group.obj",
    "../models/smoothing-normal.obj",
    "../models/issue-295-trianguation-failure.obj",
    NULL};

static void test_corpus_loads(void) {
  for (int i = 0; kCorpus[i]; i++) {
    tobj_scene_f sc;
    tobj_result r = load_file_f(kCorpus[i], &sc);
    TEST_CHECK_(r == TOBJ_OK, "%s -> %s", kCorpus[i], tobj_result_string(r));
    if (r == TOBJ_OK) {
      TEST_CHECK_(mesh_consistent_f(&sc), "%s mesh consistent", kCorpus[i]);
      tobj_scene_free_f(&sc);
    }
  }
}

static void test_both_precisions(void) {
  tobj_scene_f sf;
  tobj_scene_d sd;
  tobj_load_config cfg = tobj_default_config();
  TEST_CHECK(tobj_load_obj_from_file_f(&sf, "../models/cube.obj", &cfg, NULL) ==
             TOBJ_OK);
  TEST_CHECK(tobj_load_obj_from_file_d(&sd, "../models/cube.obj", &cfg, NULL) ==
             TOBJ_OK);
  /* identical topology across precision families */
  TEST_CHECK(sf.num_shapes == sd.num_shapes);
  TEST_CHECK(sf.attrib.vertices.count == sd.attrib.vertices.count);
  size_t fi = 0, di = 0;
  for (size_t s = 0; s < sf.num_shapes; s++) fi += sf.shapes[s].mesh.num_indices;
  for (size_t s = 0; s < sd.num_shapes; s++) di += sd.shapes[s].mesh.num_indices;
  TEST_CHECK(fi == di);
  tobj_scene_free_f(&sf);
  tobj_scene_free_d(&sd);
}

/* ---- strict index validation ------------------------------------------- */

/* Strict mode must accept a well-formed model and produce exactly the same
 * geometry/topology as the lenient default. */
static void test_strict_accepts_valid(void) {
  const char *obj =
      "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvn 0 0 1\n"
      "f 1/1/1 2/1/1 3/1/1\n";
  tobj_scene_f lax, strict;
  tobj_diag diag;  memset(&diag, 0, sizeof diag);
  TEST_CHECK(load_strict_f(obj, false, &lax, NULL) == TOBJ_OK);
  TEST_CHECK(load_strict_f(obj, true, &strict, &diag) == TOBJ_OK);
  TEST_CHECK(diag.err == NULL && diag.warn == NULL);
  TEST_CHECK(lax.num_shapes == 1 && strict.num_shapes == 1);
  TEST_CHECK(lax.shapes[0].mesh.num_indices == strict.shapes[0].mesh.num_indices);
  TEST_CHECK(lax.attrib.vertices.count == strict.attrib.vertices.count);
  for (size_t i = 0; i < strict.shapes[0].mesh.num_indices; i++) {
    TEST_CHECK(lax.shapes[0].mesh.indices[i].vertex_index ==
               strict.shapes[0].mesh.indices[i].vertex_index);
    TEST_CHECK(lax.shapes[0].mesh.indices[i].texcoord_index ==
               strict.shapes[0].mesh.indices[i].texcoord_index);
    TEST_CHECK(lax.shapes[0].mesh.indices[i].normal_index ==
               strict.shapes[0].mesh.indices[i].normal_index);
  }
  tobj_diag_free(&diag, NULL);
  tobj_scene_free_f(&lax);
  tobj_scene_free_f(&strict);
}

/* Missing optional attributes are "absent", never negative indices: a face
 * without vt/vn is legal under strict validation and keeps -1 sentinels. */
static void test_strict_absent_optional_is_not_error(void) {
  const char *obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
  tobj_scene_f sc;
  tobj_diag diag;
  memset(&diag, 0, sizeof diag);
  TEST_CHECK(load_strict_f(obj, true, &sc, &diag) == TOBJ_OK);
  TEST_CHECK(diag.err == NULL);
  TEST_CHECK(sc.shapes[0].mesh.indices[0].texcoord_index == -1);
  TEST_CHECK(sc.shapes[0].mesh.indices[0].normal_index == -1);
  tobj_diag_free(&diag, NULL);
  tobj_scene_free_f(&sc);
}

/* Positive out-of-range vertex reference: strict fails and points at the line;
 * the scene must not look like a usable mesh afterwards. */
static void test_strict_rejects_vertex_out_of_range(void) {
  const char *obj = "v 0 0 0\nv 1 0 0\nf 1 2 4\n";
  tobj_scene_f sc;
  tobj_diag diag;
  memset(&diag, 0, sizeof diag);
  TEST_CHECK(load_strict_f(obj, true, &sc, &diag) == TOBJ_ERR_PARSE);
  TEST_CHECK(sc.num_shapes == 0);
  TEST_CHECK(sc.attrib.vertices.count == 0);
  TEST_CHECK(diag.err && strstr(diag.err, "line 3"));
  TEST_CHECK(diag.err && strstr(diag.err, "corner 3 of 3"));
  TEST_CHECK(diag.err && strstr(diag.err, "vertex"));

  /* The same model under the default lenient mode still loads. */
  tobj_scene_f lax;
  TEST_CHECK(load_strict_f(obj, false, &lax, NULL) == TOBJ_OK);
  TEST_CHECK(lax.num_shapes == 1);
  tobj_scene_free_f(&lax);
  tobj_diag_free(&diag, NULL);
}

/* Negative (relative) reference that reaches before the start of the data. */
static void test_strict_rejects_relative_before_start(void) {
  const char *obj = "v 0 0 0\nf -2 -1 1\n";
  tobj_scene_f sc;
  tobj_diag diag;
  memset(&diag, 0, sizeof diag);
  TEST_CHECK(load_strict_f(obj, true, &sc, &diag) == TOBJ_ERR_PARSE);
  TEST_CHECK(diag.err && strstr(diag.err, "line 2"));
  TEST_CHECK(diag.err && strstr(diag.err, "vertex index -2"));
  tobj_diag_free(&diag, NULL);
}

/* A valid negative reference must stay valid in strict mode. */
static void test_strict_accepts_relative_index(void) {
  const char *obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf -3 -2 -1\n";
  tobj_scene_f sc;
  TEST_CHECK(load_strict_f(obj, true, &sc, NULL) == TOBJ_OK);
  TEST_CHECK(sc.shapes[0].mesh.indices[0].vertex_index == 0);
  TEST_CHECK(sc.shapes[0].mesh.indices[2].vertex_index == 2);
  tobj_scene_free_f(&sc);
}

/* Texcoord and normal references are validated too, for positive and
 * relative forms, and the diagnostic names the offending field. */
static void test_strict_rejects_texcoord_and_normal(void) {
  tobj_scene_f sc;
  tobj_diag diag;

  const char *vt_obj =
      "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nf 1/1 2/2 3/1\n";
  memset(&diag, 0, sizeof diag);
  TEST_CHECK(load_strict_f(vt_obj, true, &sc, &diag) == TOBJ_ERR_PARSE);
  TEST_CHECK(diag.err && strstr(diag.err, "texcoord"));
  TEST_CHECK(diag.err && strstr(diag.err, "line 5"));
  tobj_diag_free(&diag, NULL);

  const char *vn_obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nf 1//1 2//2 3//1\n";
  memset(&diag, 0, sizeof diag);
  TEST_CHECK(load_strict_f(vn_obj, true, &sc, &diag) == TOBJ_ERR_PARSE);
  TEST_CHECK(diag.err && strstr(diag.err, "normal"));
  tobj_diag_free(&diag, NULL);

  /* relative texcoord that resolves is fine; one that runs off the front is not */
  const char *vt_rel_ok = "vt 0 0\nvt 1 0\nvt 0 1\n"
                          "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                          "f 1/-3 2/-2 3/-1\n";
  TEST_CHECK(load_strict_f(vt_rel_ok, true, &sc, NULL) == TOBJ_OK);
  TEST_CHECK(sc.shapes[0].mesh.indices[0].texcoord_index == 0);
  TEST_CHECK(sc.shapes[0].mesh.indices[1].texcoord_index == 1);
  TEST_CHECK(sc.shapes[0].mesh.indices[2].texcoord_index == 2);
  tobj_scene_free_f(&sc);

  const char *vt_rel_bad = "vt 0 0\nv 0 0 0\nv 1 0 0\nv 0 1 0\n"
                           "f 1/-2 2/-1 3/-1\n";
  memset(&diag, 0, sizeof diag);
  TEST_CHECK(load_strict_f(vt_rel_bad, true, &sc, &diag) == TOBJ_ERR_PARSE);
  tobj_diag_free(&diag, NULL);
}

/* Index 0 is invalid per the format; strict rejects it while the lenient
 * default keeps its historical -1 / warning behaviour. */
static void test_strict_rejects_zero_index(void) {
  const char *obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 0 1 2\n";
  tobj_scene_f sc;
  tobj_diag diag;
  memset(&diag, 0, sizeof diag);
  TEST_CHECK(load_strict_f(obj, true, &sc, &diag) == TOBJ_ERR_PARSE);
  TEST_CHECK(diag.err && strstr(diag.err, "zero"));
  tobj_diag_free(&diag, NULL);

  tobj_diag laxdiag;
  memset(&laxdiag, 0, sizeof laxdiag);
  TEST_CHECK(load_strict_f(obj, false, &sc, &laxdiag) == TOBJ_OK);
  TEST_CHECK(sc.shapes[0].mesh.indices[0].vertex_index == -1);
  TEST_CHECK(laxdiag.warn && strstr(laxdiag.warn, "invalid vertex index"));
  tobj_scene_free_f(&sc);
  tobj_diag_free(&laxdiag, NULL);
}

/* Faces may mix positive and negative references; a valid mix must resolve
 * to the same vertex/texcoord/normal for both spellings of every corner. */
static void test_strict_mixed_positive_and_relative(void) {
  const char *obj =
      "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
      "vt 0 0\nvt 1 0\nvt 0 1\n"
      "vn 0 0 1\nvn 0 0 1\nvn 0 0 1\n"
      "f 1/1/1 2/2/2 3/3/3\n"
      "f -3/-3/-3 -2/-2/-2 -1/-1/-1\n";
  tobj_scene_f sc;
  TEST_CHECK(load_strict_f(obj, true, &sc, NULL) == TOBJ_OK);
  TEST_CHECK(sc.shapes[0].mesh.num_indices == 6);
  for (int f = 0; f < 2; f++) {
    for (int k = 0; k < 3; k++) {
      const tobj_index *ix = &sc.shapes[0].mesh.indices[f * 3 + k];
      TEST_CHECK(ix->vertex_index == k);
      TEST_CHECK(ix->texcoord_index == k);
      TEST_CHECK(ix->normal_index == k);
    }
  }
  tobj_scene_free_f(&sc);
}

/* Lines and points validate their references the same way. */
static void test_strict_lines_and_points(void) {
  const char *good = "v 0 0 0\nv 1 0 0\nv 2 0 0\nl 1 2 -3\np -1\n";
  tobj_scene_f sc;
  TEST_CHECK(load_strict_f(good, true, &sc, NULL) == TOBJ_OK);
  TEST_CHECK(sc.shapes[0].lines.num_indices == 3);
  TEST_CHECK(sc.shapes[0].points.num_indices == 1);
  TEST_CHECK(sc.shapes[0].points.indices[0].vertex_index == 2);
  tobj_scene_free_f(&sc);

  const char *bad_line = "v 0 0 0\nv 1 0 0\nl 1 5\n";
  tobj_diag diag;
  memset(&diag, 0, sizeof diag);
  TEST_CHECK(load_strict_f(bad_line, true, &sc, &diag) == TOBJ_ERR_PARSE);
  TEST_CHECK(diag.err && strstr(diag.err, "line at line 3"));
  tobj_diag_free(&diag, NULL);

  const char *bad_point = "v 0 0 0\np 3\n";
  memset(&diag, 0, sizeof diag);
  TEST_CHECK(load_strict_f(bad_point, true, &sc, &diag) == TOBJ_ERR_PARSE);
  TEST_CHECK(diag.err && strstr(diag.err, "point"));
  tobj_diag_free(&diag, NULL);
}

/* The same guarantees hold on the streaming-callback entry point. */
typedef struct cb_counts {
  int vertex, face, line, point;
} cb_counts;

static void cb_vertex(void *ud, float x, float y, float z, float w) {
  (void)x;
  (void)y;
  (void)z;
  (void)w;
  ((cb_counts *)ud)->vertex++;
}
static void cb_index(void *ud, const tobj_index *i, int n) {
  (void)i;
  (void)n;
  ((cb_counts *)ud)->face++;
}
static void cb_line(void *ud, const tobj_index *i, int n) {
  (void)i;
  (void)n;
  ((cb_counts *)ud)->line++;
}
static void cb_point(void *ud, const tobj_index *i, int n) {
  (void)i;
  (void)n;
  ((cb_counts *)ud)->point++;
}

static void test_strict_streaming_callbacks(void) {
  const char *obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
  tobj_callbacks_f cb;
  memset(&cb, 0, sizeof cb);
  cb_counts counts;
  memset(&counts, 0, sizeof counts);
  cb.vertex_cb = cb_vertex;
  cb.index_cb = cb_index;
  cb.line_cb = cb_line;
  cb.point_cb = cb_point;
  cb.user_data = &counts;

  tobj_load_config cfg = tobj_default_config();
  cfg.strict_index_validation = true;
  tobj_diag diag;
  memset(&diag, 0, sizeof diag);
  TEST_CHECK(tobj_load_obj_with_callbacks_f(
                 (const uint8_t *)obj, strlen(obj), &cb, &cfg, &diag) ==
             TOBJ_OK);
  TEST_CHECK(counts.face == 1);
  tobj_diag_free(&diag, NULL);

  const char *bad = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 4\n";
  memset(&counts, 0, sizeof counts);
  memset(&diag, 0, sizeof diag);
  TEST_CHECK(tobj_load_obj_with_callbacks_f(
                 (const uint8_t *)bad, strlen(bad), &cb, &cfg, &diag) ==
             TOBJ_ERR_PARSE);
  TEST_CHECK(counts.face == 0); /* the bad face is not delivered */
  TEST_CHECK(diag.err && strstr(diag.err, "line 4"));
  tobj_diag_free(&diag, NULL);
}

/* The lenient streaming loader resolves bad references against its own running
 * counters (not the scene builder's, which stays empty on this path), matching
 * what it did before the strict option existed. A zero vertex index becomes
 * -1 and is still delivered; a relative reference resolves against the
 * streamed vertex count. */
typedef struct cb_first {
  tobj_index first;
  int n;
} cb_first;

static void cb_first_index(void *ud, const tobj_index *i, int n) {
  cb_first *f = (cb_first *)ud;
  if (f->n == 0 && n > 0) f->first = i[0];
  f->n++;
}

static void test_lenient_streaming_keeps_legacy_indices(void) {
  const char *zero_obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 0 1 2\n";
  const char *rel_obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf -3 -2 -1\n";
  tobj_callbacks_f cb;
  memset(&cb, 0, sizeof cb);
  cb.index_cb = cb_first_index;

  tobj_load_config cfg = tobj_default_config(); /* strict off */

  cb_first seen;
  memset(&seen, 0, sizeof seen);
  cb.user_data = &seen;
  TEST_CHECK(tobj_load_obj_with_callbacks_f(
                 (const uint8_t *)zero_obj, strlen(zero_obj), &cb, &cfg,
                 NULL) == TOBJ_OK);
  TEST_CHECK(seen.n == 1);
  TEST_CHECK(seen.first.vertex_index == -1);
  TEST_CHECK(seen.first.texcoord_index == -1);
  TEST_CHECK(seen.first.normal_index == -1);

  memset(&seen, 0, sizeof seen);
  TEST_CHECK(tobj_load_obj_with_callbacks_f(
                 (const uint8_t *)rel_obj, strlen(rel_obj), &cb, &cfg, NULL) ==
             TOBJ_OK);
  TEST_CHECK(seen.n == 1);
  TEST_CHECK(seen.first.vertex_index == 0);
}

/* Free-form control points are index references too. */
static void test_strict_freeform_surface(void) {
  const char *obj = "v 0 0 0\nv 1 0 0\ncstype bezier\ndeg 1 1\n"
                    "surf 0 1 0 1 1 2 3\n";
  tobj_load_config cfg = tobj_default_config();
  cfg.strict_index_validation = true;
  cfg.parse_freeform = true;
  tobj_scene_f sc;
  tobj_diag diag;
  memset(&diag, 0, sizeof diag);
  TEST_CHECK(tobj_load_obj_from_memory_f(
                 &sc, (const uint8_t *)obj, strlen(obj), &cfg, &diag) ==
             TOBJ_ERR_PARSE);
  TEST_CHECK(diag.err && strstr(diag.err, "surf"));
  tobj_diag_free(&diag, NULL);
}

/* A large model with cross-face mixed indexing must produce identical results
 * with strict validation on, on the multithreaded and single-threaded paths
 * alike (the MT build resolves relative indices against running counts). */
static void test_strict_large_mixed_model(void) {
  enum { NV = 4000, NF = 1200 };
  static char obj[NV * 16 + NF * 64];
  size_t n = 0;
  for (int i = 0; i < NV; i++) n += (size_t)sprintf(obj + n, "v %d 0 0\n", i);
  for (int f = 0; f < NF; f++) {
    int base = f % (NV - 3);
    if ((f & 1) == 0)
      n += (size_t)sprintf(obj + n, "f %d %d %d\n", base + 1, base + 2,
                           base + 3);
    else
      n += (size_t)sprintf(obj + n, "f -%d -%d -%d\n", NV - base, NV - base - 1,
                           NV - base - 2);
  }
  tobj_load_config cfg = tobj_default_config();
  cfg.strict_index_validation = true;
  tobj_scene_f sc;
  TEST_CHECK(tobj_load_obj_from_memory_f(&sc, (const uint8_t *)obj, n, &cfg,
                                         NULL) == TOBJ_OK);
  size_t numv = sc.attrib.vertices.count / 3;
  TEST_CHECK(numv == NV);
  size_t total = 0;
  for (size_t s = 0; s < sc.num_shapes; s++) total += sc.shapes[s].mesh.num_indices;
  TEST_CHECK(total == (size_t)NF * 3);
  for (size_t s = 0; s < sc.num_shapes; s++) {
    for (size_t i = 0; i < sc.shapes[s].mesh.num_indices; i++) {
      int vi = sc.shapes[s].mesh.indices[i].vertex_index;
      TEST_CHECK(vi >= 0 && (size_t)vi < numv);
    }
  }
  tobj_scene_free_f(&sc);
}

/* ---- strict index validation end --------------------------------------- */


static void test_quad_triangulation(void) {
  tobj_scene_f sc;
  const char *obj = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\n";
  load_string_f(obj, &sc, true);
  TEST_CHECK(sc.num_shapes == 1);
  TEST_CHECK(sc.shapes[0].mesh.num_faces == 2);   /* 2 triangles */
  TEST_CHECK(sc.shapes[0].mesh.num_indices == 6); /* 6 indices */
  tobj_scene_free_f(&sc);
}

static void test_no_triangulation(void) {
  tobj_scene_f sc;
  const char *obj = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\n";
  load_string_f(obj, &sc, false);
  TEST_CHECK(sc.shapes[0].mesh.num_faces == 1);
  TEST_CHECK(sc.shapes[0].mesh.num_face_vertices[0] == 4);
  TEST_CHECK(sc.shapes[0].mesh.num_indices == 4);
  tobj_scene_free_f(&sc);
}

static void test_negative_relative_index(void) {
  tobj_scene_f sc;
  /* -1 -2 -3 refer to the three most recent vertices */
  const char *obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf -3 -2 -1\n";
  load_string_f(obj, &sc, true);
  TEST_CHECK(sc.shapes[0].mesh.num_indices == 3);
  TEST_CHECK(sc.shapes[0].mesh.indices[0].vertex_index == 0);
  TEST_CHECK(sc.shapes[0].mesh.indices[1].vertex_index == 1);
  TEST_CHECK(sc.shapes[0].mesh.indices[2].vertex_index == 2);
  tobj_scene_free_f(&sc);
}

static void test_bom_and_crlf(void) {
  tobj_scene_f sc;
  const char obj[] = "\xEF\xBB\xBFv 0 0 0\r\nv 1 0 0\r\nv 0 1 0\r\nf 1 2 3\r\n";
  tobj_load_config cfg = tobj_default_config();
  tobj_result r = tobj_load_obj_from_memory_f(&sc, (const uint8_t *)obj,
                                              sizeof(obj) - 1, &cfg, NULL);
  TEST_CHECK(r == TOBJ_OK);
  TEST_CHECK(sc.attrib.vertices.count == 9);
  TEST_CHECK(sc.shapes[0].mesh.num_indices == 3);
  tobj_scene_free_f(&sc);
}

static void test_vertex_colors(void) {
  tobj_scene_f sc;
  const char *obj = "v 0 0 0 1 0 0\nv 1 0 0 0 1 0\nv 0 1 0 0 0 1\nf 1 2 3\n";
  load_string_f(obj, &sc, true);
  TEST_CHECK(sc.attrib.colors.count == 9);
  TEST_CHECK(sc.attrib.colors.ptr[0] == 1.0f); /* red on v0 */
  TEST_CHECK(sc.attrib.colors.ptr[4] == 1.0f); /* green on v1 */
  tobj_scene_free_f(&sc);
}

static void test_color_fallback_white(void) {
  tobj_scene_f sc;
  const char *obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
  load_string_f(obj, &sc, true);
  TEST_CHECK(sc.attrib.colors.count == 9);
  for (int i = 0; i < 9; i++) TEST_CHECK(sc.attrib.colors.ptr[i] == 1.0f);
  tobj_scene_free_f(&sc);
}

static void test_lines_and_points(void) {
  tobj_scene_f sc;
  const char *obj =
      "v 0 0 0\nv 1 0 0\nv 2 0 0\nv 3 0 0\nl 1 2 3\np 1 4\n";
  load_string_f(obj, &sc, true);
  TEST_CHECK(sc.shapes[0].lines.num_lines == 1);
  TEST_CHECK(sc.shapes[0].lines.num_indices == 3);
  TEST_CHECK(sc.shapes[0].lines.num_line_vertices[0] == 3);
  TEST_CHECK(sc.shapes[0].points.num_indices == 2);
  tobj_scene_free_f(&sc);
}

static void test_degenerate_face_skipped(void) {
  tobj_scene_f sc;
  const char *obj = "v 0 0 0\nv 1 0 0\nf 1 2\nf 1 2 1\n"; /* <3 verts skipped */
  load_string_f(obj, &sc, true);
  /* the 2-vertex face is skipped; the 3-"vertex" one is kept */
  TEST_CHECK(sc.num_shapes == 1);
  tobj_scene_free_f(&sc);
}

static void test_empty_and_garbage(void) {
  tobj_scene_f sc;
  tobj_load_config cfg = tobj_default_config();
  TEST_CHECK(tobj_load_obj_from_memory_f(&sc, (const uint8_t *)"", 0, &cfg,
                                         NULL) == TOBJ_OK);
  tobj_scene_free_f(&sc);
  const char *garbage = "\x00\x01\x02 random !!! f f f\n v v v\n 99999999999";
  TEST_CHECK(tobj_load_obj_from_memory_f(&sc, (const uint8_t *)garbage, 40,
                                         &cfg, NULL) == TOBJ_OK);
  tobj_scene_free_f(&sc);
}

static void test_mtl_standalone(void) {
  tobj_material_list_f ml;
  const char *mtl =
      "newmtl m\nKd 0.1 0.2 0.3\nNs 42\nmap_Kd -s 2 2 1 tex.png\nfoo bar baz\n";
  tobj_result r =
      tobj_parse_mtl_from_memory_f(&ml, (const uint8_t *)mtl, strlen(mtl), NULL,
                                   NULL);
  TEST_CHECK(r == TOBJ_OK);
  TEST_CHECK(ml.count == 1);
  TEST_CHECK(ml.items[0].diffuse[0] == 0.1f);
  TEST_CHECK(ml.items[0].shininess == 42.0f);
  TEST_CHECK(strcmp(ml.items[0].diffuse_texname, "tex.png") == 0);
  TEST_CHECK(ml.items[0].diffuse_texopt.scale[0] == 2.0f);
  const char *foo = tobj_material_get_param_f(&ml.items[0], "foo");
  TEST_CHECK(foo && strcmp(foo, "bar baz") == 0);
  tobj_material_list_free_f(&ml);
}

static void test_io_callbacks(void) {
  const char *obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
  mem_io mio;
  mio.data = (const uint8_t *)obj;
  mio.len = strlen(obj);
  mio.pos = 0;
  mio.chunk = 5;
  mio.closed = 0;
  tobj_io_callbacks io;
  io.read = mem_io_read;
  io.close = mem_io_close;
  io.user_data = &mio;
  tobj_load_config cfg = tobj_default_config();
  tobj_scene_f sc;
  tobj_result r = tobj_load_obj_from_io_f(&sc, &io, &cfg, NULL);
  TEST_CHECK(r == TOBJ_OK);
  TEST_CHECK(mio.closed == 1);
  TEST_CHECK(sc.num_shapes == 1);
  TEST_CHECK(sc.shapes[0].mesh.num_indices == 3);
  tobj_scene_free_f(&sc);
}

static void test_allocator_limit_failure(void) {
  const char *obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
  tobj_load_config cfg = tobj_default_config();
  cfg.allocator = tobj_default_allocator();
  cfg.allocator.max_alloc_size = 16;
  tobj_scene_f sc;
  tobj_result r =
      tobj_load_obj_from_memory_f(&sc, (const uint8_t *)obj, strlen(obj), &cfg,
                                  NULL);
  TEST_CHECK(r == TOBJ_ERR_ALLOC);
  TEST_CHECK(sc.num_shapes == 0);
}

static void test_input_limit_failure(void) {
  const char *obj = "v 0 0 0\n";
  tobj_load_config cfg = tobj_default_config();
  cfg.max_input_bytes = 4;
  tobj_scene_f sc;
  tobj_result r =
      tobj_load_obj_from_memory_f(&sc, (const uint8_t *)obj, strlen(obj), &cfg,
                                  NULL);
  TEST_CHECK(r == TOBJ_ERR_LIMIT_EXCEEDED);
}

static void test_io_input_limit_exact_and_over(void) {
  const char *obj = "v 0 0 0\n";
  tobj_load_config cfg = tobj_default_config();
  cfg.max_input_bytes = strlen(obj);

  mem_io exact;
  exact.data = (const uint8_t *)obj;
  exact.len = strlen(obj);
  exact.pos = 0;
  exact.chunk = 3;
  exact.closed = 0;
  tobj_io_callbacks io;
  io.read = mem_io_read;
  io.close = mem_io_close;
  io.user_data = &exact;
  tobj_scene_f sc;
  tobj_result r = tobj_load_obj_from_io_f(&sc, &io, &cfg, NULL);
  TEST_CHECK(r == TOBJ_OK);
  TEST_CHECK(exact.closed == 1);
  tobj_scene_free_f(&sc);

  const char *too_big = "v 0 0 0\n# extra\n";
  mem_io over;
  over.data = (const uint8_t *)too_big;
  over.len = strlen(too_big);
  over.pos = 0;
  over.chunk = 0;
  over.closed = 0;
  io.user_data = &over;
  r = tobj_load_obj_from_io_f(&sc, &io, &cfg, NULL);
  TEST_CHECK(r == TOBJ_ERR_LIMIT_EXCEEDED);
  TEST_CHECK(over.closed == 1);
  TEST_CHECK(sc.num_shapes == 0);
}

static void test_io_callback_errors(void) {
  tobj_io_callbacks io;
  io.close = mem_io_close;

  mem_io state;
  state.data = NULL;
  state.len = 0;
  state.pos = 0;
  state.chunk = 0;
  state.closed = 0;
  io.user_data = &state;
  io.read = io_overreport_read;
  tobj_scene_f sc;
  tobj_result r = tobj_load_obj_from_io_f(&sc, &io, NULL, NULL);
  TEST_CHECK(r == TOBJ_ERR_IO);
  TEST_CHECK(state.closed == 1);

  state.closed = 0;
  io.read = io_error_read;
  r = tobj_load_obj_from_io_f(&sc, &io, NULL, NULL);
  TEST_CHECK(r == TOBJ_ERR_IO);
  TEST_CHECK(state.closed == 1);
}

static void test_invalid_allocator_rejected(void) {
  const char *obj = "v 0 0 0\n";
  tobj_load_config cfg = tobj_default_config();
  cfg.allocator = tobj_default_allocator();
  cfg.allocator.free = NULL;
  tobj_scene_f sc;
  tobj_result r =
      tobj_load_obj_from_memory_f(&sc, (const uint8_t *)obj, strlen(obj), &cfg,
                                  NULL);
  TEST_CHECK(r == TOBJ_ERR_INVALID_ARG);
  TEST_CHECK(sc.num_shapes == 0);
}

static void test_allocator_without_calloc_or_realloc(void) {
  const char *obj =
      "v 0 0 0\nv 1 0 0\nv 2 0 0\nv 3 0 0\nv 4 0 0\n"
      "v 5 0 0\nv 6 0 0\nv 7 0 0\nv 8 0 0\n"
      "f 1 2 3\nf 4 5 6\nf 7 8 9\n";
  tobj_load_config cfg = tobj_default_config();
  cfg.allocator = tobj_default_allocator();
  cfg.allocator.calloc = NULL;
  cfg.allocator.realloc = NULL;
  tobj_scene_f sc;
  tobj_result r =
      tobj_load_obj_from_memory_f(&sc, (const uint8_t *)obj, strlen(obj), &cfg,
                                  NULL);
  TEST_CHECK(r == TOBJ_OK);
  TEST_CHECK(sc.attrib.vertices.count == 27);
  TEST_CHECK(sc.shapes[0].mesh.num_indices == 9);
  tobj_scene_free_f(&sc);
}

TEST_LIST = {
    {"corpus_loads", test_corpus_loads},
    {"both_precisions", test_both_precisions},
    {"strict_accepts_valid", test_strict_accepts_valid},
    {"strict_absent_optional_is_not_error",
     test_strict_absent_optional_is_not_error},
    {"strict_rejects_vertex_out_of_range",
     test_strict_rejects_vertex_out_of_range},
    {"strict_rejects_relative_before_start",
     test_strict_rejects_relative_before_start},
    {"strict_accepts_relative_index", test_strict_accepts_relative_index},
    {"strict_rejects_texcoord_and_normal",
     test_strict_rejects_texcoord_and_normal},
    {"strict_rejects_zero_index", test_strict_rejects_zero_index},
    {"strict_mixed_positive_and_relative",
     test_strict_mixed_positive_and_relative},
    {"strict_lines_and_points", test_strict_lines_and_points},
    {"strict_streaming_callbacks", test_strict_streaming_callbacks},
    {"lenient_streaming_keeps_legacy_indices",
     test_lenient_streaming_keeps_legacy_indices},
    {"strict_freeform_surface", test_strict_freeform_surface},
    {"strict_large_mixed_model", test_strict_large_mixed_model},
    {"quad_triangulation", test_quad_triangulation},
    {"no_triangulation", test_no_triangulation},
    {"negative_relative_index", test_negative_relative_index},
    {"bom_and_crlf", test_bom_and_crlf},
    {"vertex_colors", test_vertex_colors},
    {"color_fallback_white", test_color_fallback_white},
    {"lines_and_points", test_lines_and_points},
    {"degenerate_face_skipped", test_degenerate_face_skipped},
    {"empty_and_garbage", test_empty_and_garbage},
    {"mtl_standalone", test_mtl_standalone},
    {"io_callbacks", test_io_callbacks},
    {"allocator_limit_failure", test_allocator_limit_failure},
    {"input_limit_failure", test_input_limit_failure},
    {"io_input_limit_exact_and_over", test_io_input_limit_exact_and_over},
    {"io_callback_errors", test_io_callback_errors},
    {"invalid_allocator_rejected", test_invalid_allocator_rejected},
    {"allocator_without_calloc_or_realloc",
     test_allocator_without_calloc_or_realloc},
    {NULL, NULL}};
