/*═══════════════════════════════════════════════════════════════════
 *  RaptorScope  -  3D Mesh Parsers (Room, Door, EMD)
 *═══════════════════════════════════════════════════════════════════*/
#include "formats/mesh.h"
#include "formats/scd.h"
#include "formats/dc2.h"
#include "core/lzss.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifndef _WIN32
#define _snprintf snprintf
#endif

#define MAX_COORD 15000

/*─── Color averaging helper ─────────────────────────────────────*/
static RGBA8 avg_colors(const u8 cols[][3], int n)
{
    int sr = 0, sg = 0, sb = 0;
    for (int i = 0; i < n; i++) {
        sr += cols[i][0]; sg += cols[i][1]; sb += cols[i][2];
    }
    float bst = 1.6f;
    int r = (int)(sr / (float)n * bst); if (r > 255) r = 255;
    int g = (int)(sg / (float)n * bst); if (g > 255) g = 255;
    int b = (int)(sb / (float)n * bst); if (b > 255) b = 255;
    return RGBA8((u8)r, (u8)g, (u8)b, 255);
}

/*─── Gouraud corner colours ─────────────────────────────────────
   Room and door faces are Gouraud-textured (command 0x34/0x3C, 0x36/0x3E
   semi-transparent; 41% of placed faces across the game vary corner to
   corner) and store an RGB per vertex; the GPU tints each texel by the
   colour interpolated across the face.  c0..c2 are the source vertices of the
   tri's corners, in the same order as its uv.  ST108's wall box is one
   grey texel shaded by corner colours 30..121. */
static void set_vcols(MeshTri& t, const u8 cols[][3], int c0, int c1, int c2)
{
    const int src[3] = { c0, c1, c2 };
    for (int k = 0; k < 3; k++)
        for (int ch = 0; ch < 3; ch++) t.vcol[k][ch] = cols[src[k]][ch];
    t.gouraud = 1;
}

/*═══════════════════════════════════════════════════════════════════
 *  Smooth Normal Computation
 *
 *  Room meshes have inline vertices per face (no shared vertex pool),
 *  so vertices at the same spatial position need their face normals
 *  accumulated and normalized to produce smooth shading.
 *
 *  Vertices that already have a non-zero normal (from EMD normal data)
 *  are left untouched — just normalized.
 *═══════════════════════════════════════════════════════════════════*/
void mesh_compute_smooth_normals(Mesh& mesh)
{
    if (!mesh.verts || !mesh.tris || mesh.tri_count == 0) return;

    /* Check if any vertex already has a non-zero normal (set by parser).
       Must check BEFORE computing anything to avoid false positives. */
    bool has_parsed_normals = false;
    for (int i = 0; i < mesh.vert_count; i++) {
        const MeshVert& v = mesh.verts[i];
        if (v.nx != 0.0f || v.ny != 0.0f || v.nz != 0.0f) {
            has_parsed_normals = true;
            break;
        }
    }

    if (has_parsed_normals) {
        /* EMD normals already set by parser — just normalize them */
        for (int i = 0; i < mesh.vert_count; i++) {
            MeshVert& v = mesh.verts[i];
            float len = sqrtf(v.nx*v.nx + v.ny*v.ny + v.nz*v.nz);
            if (len > 0.0001f) { v.nx /= len; v.ny /= len; v.nz /= len; }
        }
        return;
    }

    /* Room mesh path: spatial grouping for smooth normals.
       Room meshes duplicate vertices per face, so we find all vertices
       at the same spatial position and accumulate face normals across them. */

    /* Pre-compute face normals */
    int nf = mesh.tri_count;
    f32* fn = (f32*)calloc(nf * 3, sizeof(f32));
    if (!fn) return;
    for (int i = 0; i < nf; i++) {
        const MeshTri& t = mesh.tris[i];
        if ((int)t.idx[0] >= mesh.vert_count ||
            (int)t.idx[1] >= mesh.vert_count ||
            (int)t.idx[2] >= mesh.vert_count) continue;
        const MeshVert& v0 = mesh.verts[t.idx[0]];
        const MeshVert& v1 = mesh.verts[t.idx[1]];
        const MeshVert& v2 = mesh.verts[t.idx[2]];
        float ax = v1.x-v0.x, ay = v1.y-v0.y, az = v1.z-v0.z;
        float bx = v2.x-v0.x, by = v2.y-v0.y, bz = v2.z-v0.z;
        fn[i*3+0] = ay*bz-az*by;
        fn[i*3+1] = az*bx-ax*bz;
        fn[i*3+2] = ax*by-ay*bx;
    }

    /* For each vertex, find all faces that share its position and accumulate */
    const float eps = 0.01f;
    for (int vi = 0; vi < mesh.vert_count; vi++) {
        MeshVert& v = mesh.verts[vi];
        float sx = 0, sy = 0, sz = 0;
        for (int fi = 0; fi < nf; fi++) {
            const MeshTri& t = mesh.tris[fi];
            for (int ci = 0; ci < 3; ci++) {
                if ((int)t.idx[ci] >= mesh.vert_count) continue;
                const MeshVert& ov = mesh.verts[t.idx[ci]];
                if (fabsf(ov.x - v.x) < eps &&
                    fabsf(ov.y - v.y) < eps &&
                    fabsf(ov.z - v.z) < eps) {
                    sx += fn[fi*3+0];
                    sy += fn[fi*3+1];
                    sz += fn[fi*3+2];
                    break;  /* only add this face once */
                }
            }
        }
        float len = sqrtf(sx*sx + sy*sy + sz*sz);
        if (len > 0.0001f) {
            v.nx = sx / len; v.ny = sy / len; v.nz = sz / len;
        }
    }
    free(fn);
}

/*═══════════════════════════════════════════════════════════════════
 *  RDT Layout Parser  -  deterministic scene structure walk
 *
 *  Layout:
 *    0x00..0x1B   7 x u32 PSX pointers (metadata table refs)
 *    0x1C..N      Room mesh sections (sequential, self-terminating)
 *    N..M         EMD character models (contiguous, zero gap)
 *    M..EOF       Animation tables, RDT metadata
 *═══════════════════════════════════════════════════════════════════*/

/* Validate that an EMD header at 'off' in 'dec' of 'sz' bytes is legit */
static bool validate_emd_at(const u8* dec, size_t sz, size_t off, u32 base,
                            int* out_nv, int* out_nt, int* out_nq, int* out_np,
                            size_t* out_end)
{
    if (off + 24 > sz) return false;
    u32 ptrs[4];
    for (int i = 0; i < 4; i++)
        ptrs[i] = rd_u32(dec + off + i * 4);

    /* All 4 pointers must resolve within the data */
    u32 t[4];
    for (int i = 0; i < 4; i++) {
        if (ptrs[i] < base || ptrs[i] >= base + (u32)sz)
            return false;
        t[i] = ptrs[i] - base;
    }
    /* Must be ascending: vert < norm < tri < quad */
    if (!(t[0] < t[1] && t[1] < t[2] && t[2] < t[3]))
        return false;

    int nv = (int)(t[1] - t[0]) / 8;
    int nn = (int)(t[2] - t[1]) / 8;
    if (nv != nn || nv < 10 || nv > 5000) return false;

    if (off + 22 > sz) return false;
    u16 tt = rd_u16(dec + off + 16);
    u16 tq = rd_u16(dec + off + 18);
    u16 np = rd_u16(dec + off + 20);
    if (tt < 1 || tt > 5000 || tq > 5000) return false;
    if (np < 1 || np > 50) return false;
    /* tri_ptr + tri_count*16 must equal quad_ptr */
    if (t[2] + (u32)tt * 16 != t[3]) return false;
    /* Part table must fit between header end and vert_ptr */
    size_t hdr_end = off + 24 + (size_t)np * 20;
    if (hdr_end > t[0]) return false;

    *out_nv = nv;
    *out_nt = (int)tt;
    *out_nq = (int)tq;
    *out_np = (int)np;
    *out_end = (size_t)(t[3] + (size_t)tq * 20);
    return true;
}

bool parse_rdt_layout(const u8* dec, size_t dec_size, u32 base,
                      RdtLayout& layout)
{
    layout = RdtLayout();  /* reset all fields via constructor */
    layout.base = base;

    if (dec_size < 0x20) return false;

    /* ─── Phase 0: Check if this looks like an RDT (7-ptr header) ─── */
    bool has_rdt_header = false;
    {
        u32 p0 = rd_u32(dec);
        u32 p0_off = p0 - base;
        if (p0_off > 0x40 && p0_off < dec_size) {
            /* All 7 pointers should resolve within the file */
            int valid_ptrs = 0;
            for (int i = 0; i < 7; i++) {
                layout.hdr_ptrs[i] = rd_u32(dec + i * 4);
                u32 off = layout.hdr_ptrs[i] - base;
                if (off < dec_size) valid_ptrs++;
            }
            has_rdt_header = (valid_ptrs >= 5);
        }
    }

    /* ─── Phase 1: Walk room mesh sections starting at 0x1C ─── */
    size_t off = has_rdt_header ? 0x1C : 0;
    layout.section_count = 0;
    layout.total_room_tris = 0;
    layout.total_room_quads = 0;

    while (off + 12 <= dec_size && layout.section_count < RDT_MAX_SECTIONS) {
        u32 p1_raw = rd_u32(dec + off);
        u32 p2_raw = rd_u32(dec + off + 4);
        u16 c1 = rd_u16(dec + off + 8);
        u16 c2 = rd_u16(dec + off + 10);

        /* Validate pointers */
        if (p1_raw < base || p2_raw < base) break;
        u32 p1 = p1_raw - base;
        u32 p2 = p2_raw - base;
        if (p1 >= dec_size || p2 >= dec_size) break;
        if (c1 > 5000 || c2 > 5000) break;

        /* Validate data fits */
        size_t tri_end  = (size_t)p1 + (size_t)c1 * 40;
        size_t quad_end = (size_t)p2 + (size_t)c2 * 52;
        if (tri_end > dec_size || quad_end > dec_size) break;

        /* Empty section: skip header only */
        if (c1 == 0 && c2 == 0) { off += 12; continue; }

        /* The first face's GPU command must be a polygon of the right shape:
           0x20-0x3F, bit 3 set for quads.  Any shading, texturing or
           semi-transparency (e.g. 0x36, ST301) is allowed. */
        if (c1 > 0) {
            u8 code = dec[p1 + 24 + 7]; /* tail[7] of first tri */
            if ((code & 0xE8) != 0x20) break;
        } else if (c2 > 0) {
            u8 code = dec[p2 + 32 + 7]; /* tail[7] of first quad */
            if ((code & 0xE8) != 0x28) break;
        }

        RdtMeshSection& sec = layout.sections[layout.section_count++];
        sec.offset = off;
        sec.tri_count = c1;
        sec.quad_count = c2;
        layout.total_room_tris += c1;
        layout.total_room_quads += c2;

        off = quad_end;
    }

    layout.room_mesh_end = off;

    /* ─── Phase 2: Walk EMD models contiguously after room mesh ─── */
    layout.emd_count = 0;
    size_t emd_scan = layout.room_mesh_end;

    while (emd_scan + 24 <= dec_size && layout.emd_count < RDT_MAX_EMDS) {
        int nv, nt, nq, np;
        size_t emd_end;
        if (validate_emd_at(dec, dec_size, emd_scan, base,
                            &nv, &nt, &nq, &np, &emd_end)) {
            RdtEmdEntry& e = layout.emds[layout.emd_count++];
            e.offset = emd_scan;
            e.vert_count = nv;
            e.tri_count = nt;
            e.quad_count = nq;
            e.part_count = np;
            e.data_end = emd_end;
            emd_scan = emd_end; /* next EMD starts immediately */
        } else {
            break; /* no more contiguous EMDs */
        }
    }

    layout.emds_end = emd_scan;
    layout.valid = has_rdt_header || layout.section_count > 0 || layout.emd_count > 0;
    return layout.valid;
}

bool parse_rdt_layout_from_entry(const u8* data, size_t size,
                                 u16 ey, u16 ex, RdtLayout& layout)
{
    if (!(ey & 0x8000)) return false;

    Buffer dec;
    if (!lzss_decompress(data, size, dec)) return false;
    if (dec.size < 0x20) return false;

    u32 base = ((u32)(ey & 0x7FFF) << 16) | (u32)ex | 0x80000000u;
    return parse_rdt_layout(dec.data, dec.size, base, layout);
}

bool is_raw_rdt_entry(const DatEntry& e)
{
    if (e.type != DAT_DATA || !(e.y & 0x8000) || !e.data || e.size < 0x20)
        return false;
    u32 base = ((u32)(e.y & 0x7FFF) << 16) | (u32)e.x | 0x80000000u;
    RdtLayout layout;
    return parse_rdt_layout(e.data, e.size, base, layout) &&
           (layout.section_count > 0 || layout.emd_count > 0);
}

/*─── Detect room vs object mesh layout ──────────────────────────*/
static void detect_mesh_layout(const u8* dec, size_t dec_size,
                               u32 base, size_t& mesh_start, size_t& mesh_end)
{
    mesh_start = 0;
    mesh_end = dec_size;
    if (dec_size < 16) return;

    u32 p0 = rd_u32(dec);
    u32 p0_off = p0 - base;

    if (p0_off > 0 && p0_off < dec_size && p0_off > 0x100) {
        /* Room: 7 metadata ptrs at start, mesh sections after header.
           Find smallest metadata ptr that's past the header area (0x1C+).
           Only consider ptrs > 0x40 to avoid false hits on mesh section data. */
        size_t meta_min = dec_size;
        for (int si = 0; si < 7; si++) {
            if ((size_t)si * 4 + 4 > dec_size) break;
            u32 sp = rd_u32(dec + si * 4) - base;
            if (sp > 0x40 && sp < meta_min)
                meta_min = sp;
        }
        mesh_start = 0x1C;
        mesh_end = meta_min;
    }
}

/*═══════════════════════════════════════════════════════════════════
 *  Parse Room Mesh  (pre-decompressed)
 *═══════════════════════════════════════════════════════════════════*/
bool parse_room_mesh_dec(const u8* dec_data, size_t dec_size, u32 base_addr, Mesh& mesh,
                         const u32* exclude_offsets, int n_exclude)
{
    if (dec_size < 16) return false;

    /* Temporary dynamic arrays */
    Array<MeshVert> verts;
    Array<MeshTri>  tris;
    verts.reserve(4096);
    tris.reserve(4096);

    size_t mesh_start, mesh_end;
    detect_mesh_layout(dec_data, dec_size, base_addr, mesh_start, mesh_end);

    size_t off = mesh_start;
    int max_sections = 200; /* safety limit */
    while (off + 12 <= mesh_end && max_sections-- > 0) {
        /* Check if this section should be excluded */
        bool skip = false;
        for (int ei = 0; ei < n_exclude; ei++)
            if ((u32)off == exclude_offsets[ei]) { skip = true; break; }

        u32 p1 = rd_u32(dec_data + off) - base_addr;
        u32 p2 = rd_u32(dec_data + off + 4) - base_addr;
        u16 c1 = rd_u16(dec_data + off + 8);
        u16 c2 = rd_u16(dec_data + off + 10);

        /* Basic sanity: pointers must be within data */
        if (p1 >= dec_size || p2 >= dec_size) break;
        if (c1 > 5000 || c2 > 5000) break;

        /* Validate tri region */
        size_t tri_end = p1 + (size_t)c1 * 40;
        size_t quad_end = p2 + (size_t)c2 * 52;
        if (tri_end > dec_size || quad_end > dec_size) break;

        /* If c1 == 0, tri ptr should equal quad ptr (or be irrelevant) */
        if (c1 == 0 && c2 == 0) { off += 12; continue; }

        /* Skip excluded sections (still advance past them) */
        if (skip) { off = quad_end; continue; }

        /* Parse triangles (40 bytes each)  -  use p1 directly */
        for (int i = 0; i < (int)c1; i++) {
            size_t o = p1 + (size_t)i * 40;
            if (o + 40 > dec_size) break;

            s16 vs[3][3];
            u8  uvs[3][2];
            for (int vi = 0; vi < 3; vi++) {
                vs[vi][0] = rd_s16(dec_data + o + vi * 8);
                vs[vi][1] = rd_s16(dec_data + o + vi * 8 + 2);
                vs[vi][2] = rd_s16(dec_data + o + vi * 8 + 4);
                u16 uv_raw = rd_u16(dec_data + o + vi * 8 + 6);
                uvs[vi][0] = (u8)(uv_raw & 0xFF);
                uvs[vi][1] = (u8)((uv_raw >> 8) & 0xFF);
            }

            u16 tp = rd_u16(dec_data + o + 24);
            u16 cl = rd_u16(dec_data + o + 26);
            u8 cols[3][3];
            for (int ci = 0; ci < 3; ci++) {
                cols[ci][0] = dec_data[o + 28 + ci * 4];
                cols[ci][1] = dec_data[o + 28 + ci * 4 + 1];
                cols[ci][2] = dec_data[o + 28 + ci * 4 + 2];
            }

            u32 bi = (u32)verts.count;
            for (int vi = 0; vi < 3; vi++) {
                MeshVert mv = {};
                mv.x = -(f32)vs[vi][0];
                mv.y = (f32)(-vs[vi][1]);
                mv.z = (f32)vs[vi][2];
                verts.push(mv);
            }
            MeshTri t = {};
            t.idx[0] = bi; t.idx[1] = bi+2; t.idx[2] = bi+1;  /* reversed for X negate */
            t.color = avg_colors(cols, 3);
            set_vcols(t, cols, 0, 2, 1);
            t.tpage = tp; t.clut = cl;
            t.uv[0][0] = uvs[0][0]; t.uv[0][1] = uvs[0][1];
            t.uv[1][0] = uvs[2][0]; t.uv[1][1] = uvs[2][1];
            t.uv[2][0] = uvs[1][0]; t.uv[2][1] = uvs[1][1];
            tris.push(t);
        }

        /* Parse quads (52 bytes each) */
        for (int i = 0; i < (int)c2; i++) {
            size_t o = p2 + (size_t)i * 52;
            if (o + 52 > dec_size) break;

            s16 vs[4][3];
            u8  uvs[4][2];
            for (int vi = 0; vi < 4; vi++) {
                vs[vi][0] = rd_s16(dec_data + o + vi * 8);
                vs[vi][1] = rd_s16(dec_data + o + vi * 8 + 2);
                vs[vi][2] = rd_s16(dec_data + o + vi * 8 + 4);
                u16 uv_raw = rd_u16(dec_data + o + vi * 8 + 6);
                uvs[vi][0] = (u8)(uv_raw & 0xFF);
                uvs[vi][1] = (u8)((uv_raw >> 8) & 0xFF);
            }

            u16 tp = rd_u16(dec_data + o + 32);
            u16 cl = rd_u16(dec_data + o + 34);
            u8 cols[4][3];
            for (int ci = 0; ci < 4; ci++) {
                cols[ci][0] = dec_data[o + 36 + ci * 4];
                cols[ci][1] = dec_data[o + 36 + ci * 4 + 1];
                cols[ci][2] = dec_data[o + 36 + ci * 4 + 2];
            }

            u32 bi = (u32)verts.count;
            RGBA8 c = avg_colors(cols, 4);
            for (int vi = 0; vi < 4; vi++) {
                MeshVert mv = {};
                mv.x = -(f32)vs[vi][0];
                mv.y = (f32)(-vs[vi][1]);
                mv.z = (f32)vs[vi][2];
                verts.push(mv);
            }
            /* Quad triangulation: PS1 strip diagonal (v1-v2), reversed for X negate */
            MeshTri t1 = {};
            t1.idx[0] = bi; t1.idx[1] = bi+2; t1.idx[2] = bi+1;
            t1.color = c; t1.tpage = tp; t1.clut = cl;
            set_vcols(t1, cols, 0, 2, 1);
            t1.uv[0][0] = uvs[0][0]; t1.uv[0][1] = uvs[0][1];
            t1.uv[1][0] = uvs[2][0]; t1.uv[1][1] = uvs[2][1];
            t1.uv[2][0] = uvs[1][0]; t1.uv[2][1] = uvs[1][1];
            tris.push(t1);

            MeshTri t2 = {};
            t2.idx[0] = bi+1; t2.idx[1] = bi+2; t2.idx[2] = bi+3;
            t2.color = c; t2.tpage = tp; t2.clut = cl;
            set_vcols(t2, cols, 1, 2, 3);
            t2.uv[0][0] = uvs[1][0]; t2.uv[0][1] = uvs[1][1];
            t2.uv[1][0] = uvs[2][0]; t2.uv[1][1] = uvs[2][1];
            t2.uv[2][0] = uvs[3][0]; t2.uv[2][1] = uvs[3][1];
            tris.push(t2);
        }

        off = quad_end;
    }

    /* Copy to mesh */
    mesh.alloc((int)verts.count, (int)tris.count);
    memcpy(mesh.verts, verts.items, verts.count * sizeof(MeshVert));
    memcpy(mesh.tris, tris.items, tris.count * sizeof(MeshTri));
    return mesh.tri_count > 0;
}


bool parse_room_mesh(const u8* data, size_t size, u32 base_addr, Mesh& mesh)
{
    Buffer dec;
    if (!lzss_decompress(data, size, dec)) return false;
    return parse_room_mesh_dec(dec.data, dec.size, base_addr, mesh);
}

/*─── Faces the game draws over a face in front of them ──────────
   The game sorts a model slot's faces by its render mode (0x23 byte 3;
   DINO.exe 0x44DA6D): mode 0 by each face's nearest vertex, mode 1 by its
   farthest, and modes 2 and 3 put the whole section at one depth (0x5F1AE0).
   0x5F2DD0 inserts each face at the head of its ordering-table bucket, and
   the quads go in before the triangles, so in a mode 2/3 section the first
   face is drawn last, on top, whatever its depth.  ST101's windows are quads
   30-37 of slot 0, 22 units behind wall quads (193 on) that have no hole for
   them: the game draws the windows, a depth buffer hides them.  Each face
   that lies just behind a parallel face the game draws before it, and is
   mostly covered by it, is moved 2 units in front of it.  Only gaps up to 40
   units and a quarter of the face's width count: ST30A's 25-wide strips 50
   apart sit side by side on screen, not over each other.  f lists the
   section's faces in the order the game adds them; v holds their vertices in
   viewer space, a quad's in PSX order (0 1 / 2 3). */
struct SecFace { u32 v0, nv; };

static void raise_recessed_faces(MeshVert* v, const SecFace* f, int n)
{
    if (n < 2) return;
    struct Plane { f32 n[3], d, width, c[3]; bool ok; };
    Plane* pl   = (Plane*)calloc(n, sizeof(Plane));
    f32*   push = (f32*)calloc(n, sizeof(f32));
    if (!pl || !push) { free(pl); free(push); return; }
    static const int loop3[3] = { 0, 1, 2 }, loop4[4] = { 0, 1, 3, 2 };

    for (int i = 0; i < n; i++) {
        Plane& p = pl[i];
        const MeshVert* q = v + f[i].v0;
        const int* loop = f[i].nv == 4 ? loop4 : loop3;
        /* Front normal: the triangles wind v0, v2, v1 */
        f32 e1[3] = { q[2].x - q[0].x, q[2].y - q[0].y, q[2].z - q[0].z };
        f32 e2[3] = { q[1].x - q[0].x, q[1].y - q[0].y, q[1].z - q[0].z };
        p.n[0] = e1[1] * e2[2] - e1[2] * e2[1];
        p.n[1] = e1[2] * e2[0] - e1[0] * e2[2];
        p.n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        f32 area = sqrtf(p.n[0] * p.n[0] + p.n[1] * p.n[1] + p.n[2] * p.n[2]);
        if (area < 0.001f) continue;
        for (int k = 0; k < 3; k++) p.n[k] /= area;
        p.d = p.n[0] * q[0].x + p.n[1] * q[0].y + p.n[2] * q[0].z;
        area *= 0.5f;
        if (f[i].nv == 4) {
            f32 g1[3] = { q[1].x - q[3].x, q[1].y - q[3].y, q[1].z - q[3].z };
            f32 g2[3] = { q[2].x - q[3].x, q[2].y - q[3].y, q[2].z - q[3].z };
            f32 cx = g1[1] * g2[2] - g1[2] * g2[1];
            f32 cy = g1[2] * g2[0] - g1[0] * g2[2];
            f32 cz = g1[0] * g2[1] - g1[1] * g2[0];
            area += 0.5f * sqrtf(cx * cx + cy * cy + cz * cz);
        }
        /* Width: area over the longest edge (a rectangle's shorter side) */
        f32 longest = 0;
        p.ok = true;
        for (u32 k = 0; k < f[i].nv; k++) {
            const MeshVert& a = q[loop[k]];
            const MeshVert& b = q[loop[(k + 1) % f[i].nv]];
            f32 dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
            f32 e = sqrtf(dx * dx + dy * dy + dz * dz);
            if (e > longest) longest = e;
            if (fabsf(p.n[0] * a.x + p.n[1] * a.y + p.n[2] * a.z - p.d) > 1.0f) p.ok = false;
            p.c[0] += a.x / f[i].nv; p.c[1] += a.y / f[i].nv; p.c[2] += a.z / f[i].nv;
        }
        p.width = area / longest;
    }

    for (int i = 0; i < n; i++) {
        const Plane& a = pl[i];
        if (!a.ok) continue;
        f32 limit = a.width / 4.0f;
        if (limit > 40.0f) limit = 40.0f;
        if (limit < 1.0f) continue;
        for (int j = i + 1; j < n; j++) {
            const Plane& b = pl[j];
            if (!b.ok) continue;
            if (a.n[0] * b.n[0] + a.n[1] * b.n[1] + a.n[2] * b.n[2] < 0.999f) continue;
            /* Behind b: every vertex 0.5..limit units on b's back side */
            f32 nearest = -1e9f, deepest = 1e9f;
            for (u32 k = 0; k < f[i].nv; k++) {
                const MeshVert& q = v[f[i].v0 + k];
                f32 s = b.n[0] * q.x + b.n[1] * q.y + b.n[2] * q.z - b.d;
                if (s > nearest) nearest = s;
                if (s < deepest) deepest = s;
            }
            if (nearest > -0.5f || deepest < -limit) continue;
            /* ...and mostly covered by b: of its centre and the points halfway
               to its corners, at least half on the inner side of b's edges */
            const MeshVert* q = v + f[j].v0;
            const int* loop = f[j].nv == 4 ? loop4 : loop3;
            u32 hits = 0;
            for (u32 s = 0; s <= f[i].nv; s++) {
                f32 pt[3] = { a.c[0], a.c[1], a.c[2] };
                if (s < f[i].nv) {
                    const MeshVert& w = v[f[i].v0 + s];
                    pt[0] = (pt[0] + w.x) * 0.5f; pt[1] = (pt[1] + w.y) * 0.5f; pt[2] = (pt[2] + w.z) * 0.5f;
                }
                bool inside = true;
                for (u32 k = 0; k < f[j].nv && inside; k++) {
                    const MeshVert& e0 = q[loop[k]];
                    const MeshVert& e1 = q[loop[(k + 1) % f[j].nv]];
                    f32 ex = e1.x - e0.x, ey = e1.y - e0.y, ez = e1.z - e0.z;
                    f32 px = pt[0] - e0.x, py = pt[1] - e0.y, pz = pt[2] - e0.z;
                    f32 side = b.n[0] * (ey * pz - ez * py) + b.n[1] * (ez * px - ex * pz) +
                               b.n[2] * (ex * py - ey * px);
                    if (side > 0) inside = false;
                }
                if (inside) hits++;
            }
            if (hits * 2 <= f[i].nv + 1) continue;
            if (2.0f - deepest > push[i]) push[i] = 2.0f - deepest;
        }
    }

    for (int i = 0; i < n; i++) {
        if (push[i] <= 0) continue;
        for (u32 k = 0; k < f[i].nv; k++) {
            MeshVert& q = v[f[i].v0 + k];
            q.x += pl[i].n[0] * push[i];
            q.y += pl[i].n[1] * push[i];
            q.z += pl[i].n[2] * push[i];
        }
    }
    free(pl);
    free(push);
}

/*═══════════════════════════════════════════════════════════════════
 *  Apply 0x23 Section Transforms (Instancing)
 *
 *  For each xform with non-zero position, re-parse the referenced
 *  sections from the RDT and append the geometry to the mesh with
 *  position offset applied. Sections already at origin are already
 *  in the mesh from parse_room_mesh_dec.
 *═══════════════════════════════════════════════════════════════════*/
void mesh_apply_xforms(Mesh& mesh, const u8* dec, size_t dec_size, u32 base,
                       const RdtSectionXform* xforms, int n_xforms)
{
    if (!xforms || n_xforms <= 0) return;

    /* Temporary buffers for new geometry */
    Array<MeshVert> new_verts;
    Array<MeshTri>  new_tris;
    Array<SecFace>  sec_faces;
    new_verts.reserve(8192);
    new_tris.reserve(8192);

    /* Metadata region end (smallest header pointer) */
    size_t meta_min = dec_size;
    for (int i = 0; i < 7; i++) {
        if (i * 4 + 4 > (int)dec_size) break;
        u32 p = rd_u32(dec + i * 4) - base;
        if (p > 0x40 && p < meta_min) meta_min = p;
    }

    for (int xi = 0; xi < n_xforms; xi++) {
        const RdtSectionXform& xf = xforms[xi];
        if (xf.section_off == 0xFFFFFFFF) continue;
        if (xf.section_off < 0x1C || xf.section_off >= meta_min) continue;

        /* Position offset (matching parse_room_mesh_dec coordinate system) */
        f32 ox = -(f32)xf.px;
        f32 oy = -(f32)xf.py;
        f32 oz =  (f32)xf.pz;

        /* Y-axis rotation (PSX: 4096 = 360°) */
        f32 cr = 1.0f, sr = 0.0f;
        bool has_rot = false;
        if (xf.ry != 0) {
            f32 angle = (f32)xf.ry * 6.2831853f / 4096.0f;
            cr = cosf(angle);
            sr = sinf(angle);
            has_rot = true;
        }

        /* Each 0x23 xform maps to exactly ONE section descriptor.
           render_mode (byte[3]) controls OT sort order, not section count. */
        size_t off = xf.section_off;
        int sec_limit = 1;
        int sec_done = 0;

        while (off + 12 <= meta_min && sec_done < sec_limit) {
            u32 p1 = rd_u32(dec + off) - base;
            u32 p2 = rd_u32(dec + off + 4) - base;
            u16 c1 = rd_u16(dec + off + 8);
            u16 c2 = rd_u16(dec + off + 10);
            if (p1 >= dec_size || p2 >= dec_size) break;
            if (c1 > 5000 || c2 > 5000) break;
            size_t tri_end = p1 + (size_t)c1 * 40;
            size_t quad_end = p2 + (size_t)c2 * 52;
            if (tri_end > dec_size || quad_end > dec_size) break;
            if (c1 == 0 && c2 == 0) { off += 12; continue; }

            /* Quads before triangles, the order the game adds them in */
            sec_faces.clear();
            for (int i = 0; i < (int)c2; i++) {
                size_t o = p2 + (size_t)i * 52;
                if (o + 52 > dec_size) break;
                s16 vs[4][3]; u8 uvs[4][2];
                for (int vi = 0; vi < 4; vi++) {
                    vs[vi][0] = rd_s16(dec + o + vi*8);
                    vs[vi][1] = rd_s16(dec + o + vi*8+2);
                    vs[vi][2] = rd_s16(dec + o + vi*8+4);
                    u16 uv_raw = rd_u16(dec + o + vi*8+6);
                    uvs[vi][0] = (u8)(uv_raw & 0xFF);
                    uvs[vi][1] = (u8)(uv_raw >> 8);
                }
                u16 tp = rd_u16(dec + o + 32);
                u16 cl = rd_u16(dec + o + 34);
                u8 solid = !(dec[o + 39] & 2);  /* command byte: bit 1 = semi-transparent */
                u8 cols[4][3];
                for (int ci = 0; ci < 4; ci++) {
                    cols[ci][0] = dec[o+36+ci*4]; cols[ci][1] = dec[o+36+ci*4+1]; cols[ci][2] = dec[o+36+ci*4+2];
                }
                u32 bi = (u32)new_verts.count;
                SecFace sf = { bi, 4 }; sec_faces.push(sf);
                RGBA8 c = avg_colors(cols, 4);
                for (int vi = 0; vi < 4; vi++) {
                    f32 lx = (f32)vs[vi][0];
                    f32 ly = (f32)vs[vi][1];
                    f32 lz = (f32)vs[vi][2];
                    if (has_rot) {
                        f32 rx2 = cr * lx + sr * lz;
                        f32 rz2 = -sr * lx + cr * lz;
                        lx = rx2; lz = rz2;
                    }
                    MeshVert mv = {};
                    mv.x = -lx + ox;
                    mv.y = -ly + oy;
                    mv.z =  lz + oz;
                    new_verts.push(mv);
                }
                MeshTri t1 = {};
                t1.idx[0]=bi; t1.idx[1]=bi+2; t1.idx[2]=bi+1;
                t1.color=c; t1.tpage=tp; t1.clut=cl; t1.solid=solid;
                set_vcols(t1, cols, 0, 2, 1);
                t1.uv[0][0]=uvs[0][0]; t1.uv[0][1]=uvs[0][1];
                t1.uv[1][0]=uvs[2][0]; t1.uv[1][1]=uvs[2][1];
                t1.uv[2][0]=uvs[1][0]; t1.uv[2][1]=uvs[1][1];
                t1.src_off = xf.section_off; t1.slot = xf.slot;
                new_tris.push(t1);
                MeshTri t2 = {};
                t2.idx[0]=bi+1; t2.idx[1]=bi+2; t2.idx[2]=bi+3;
                t2.color=c; t2.tpage=tp; t2.clut=cl; t2.solid=solid;
                set_vcols(t2, cols, 1, 2, 3);
                t2.uv[0][0]=uvs[1][0]; t2.uv[0][1]=uvs[1][1];
                t2.uv[1][0]=uvs[2][0]; t2.uv[1][1]=uvs[2][1];
                t2.uv[2][0]=uvs[3][0]; t2.uv[2][1]=uvs[3][1];
                t2.src_off = xf.section_off; t2.slot = xf.slot;
                new_tris.push(t2);
            }

            /* Then triangles */
            for (int i = 0; i < (int)c1; i++) {
                size_t o = p1 + (size_t)i * 40;
                if (o + 40 > dec_size) break;
                s16 vs[3][3]; u8 uvs[3][2];
                for (int vi = 0; vi < 3; vi++) {
                    vs[vi][0] = rd_s16(dec + o + vi*8);
                    vs[vi][1] = rd_s16(dec + o + vi*8+2);
                    vs[vi][2] = rd_s16(dec + o + vi*8+4);
                    u16 uv_raw = rd_u16(dec + o + vi*8+6);
                    uvs[vi][0] = (u8)(uv_raw & 0xFF);
                    uvs[vi][1] = (u8)(uv_raw >> 8);
                }
                u16 tp = rd_u16(dec + o + 24);
                u16 cl = rd_u16(dec + o + 26);
                u8 solid = !(dec[o + 31] & 2);
                u8 cols[3][3];
                for (int ci = 0; ci < 3; ci++) {
                    cols[ci][0] = dec[o+28+ci*4]; cols[ci][1] = dec[o+28+ci*4+1]; cols[ci][2] = dec[o+28+ci*4+2];
                }
                u32 bi = (u32)new_verts.count;
                SecFace sf = { bi, 3 }; sec_faces.push(sf);
                for (int vi = 0; vi < 3; vi++) {
                    f32 lx = (f32)vs[vi][0];
                    f32 ly = (f32)vs[vi][1];
                    f32 lz = (f32)vs[vi][2];
                    if (has_rot) {
                        f32 rx2 = cr * lx + sr * lz;
                        f32 rz2 = -sr * lx + cr * lz;
                        lx = rx2; lz = rz2;
                    }
                    MeshVert mv = {};
                    mv.x = -lx + ox;
                    mv.y = -ly + oy;
                    mv.z =  lz + oz;
                    new_verts.push(mv);
                }
                MeshTri t = {};
                t.idx[0] = bi; t.idx[1] = bi+2; t.idx[2] = bi+1;
                t.color = avg_colors(cols, 3);
                set_vcols(t, cols, 0, 2, 1);
                t.tpage = tp; t.clut = cl; t.solid = solid;
                t.uv[0][0]=uvs[0][0]; t.uv[0][1]=uvs[0][1];
                t.uv[1][0]=uvs[2][0]; t.uv[1][1]=uvs[2][1];
                t.uv[2][0]=uvs[1][0]; t.uv[2][1]=uvs[1][1];
                t.src_off = xf.section_off; t.slot = xf.slot;
                new_tris.push(t);
            }

            if (xf.render_mode == 2 || xf.render_mode == 3)
                raise_recessed_faces(new_verts.items, sec_faces.items, (int)sec_faces.count);
            off = quad_end;
            sec_done++;
        }
    }

    if (new_tris.count == 0) return;

    /* Append instanced geometry to mesh.
       New tri indices reference new_verts, offset by existing vert count. */
    int old_vc = mesh.vert_count, old_tc = mesh.tri_count;
    int total_vc = old_vc + (int)new_verts.count;
    int total_tc = old_tc + (int)new_tris.count;

    MeshVert* nv = (MeshVert*)realloc(mesh.verts, total_vc * sizeof(MeshVert));
    MeshTri*  nt = (MeshTri*) realloc(mesh.tris,  total_tc * sizeof(MeshTri));
    if (!nv || !nt) return; /* OOM */

    mesh.verts = nv;
    mesh.tris  = nt;
    memcpy(mesh.verts + old_vc, new_verts.items, new_verts.count * sizeof(MeshVert));

    /* Offset tri indices to account for existing verts */
    for (int i = 0; i < (int)new_tris.count; i++) {
        MeshTri t = new_tris.items[i];
        t.idx[0] += (u32)old_vc;
        t.idx[1] += (u32)old_vc;
        t.idx[2] += (u32)old_vc;
        mesh.tris[old_tc + i] = t;
    }
    mesh.vert_count = total_vc;
    mesh.tri_count  = total_tc;
}

/*═══════════════════════════════════════════════════════════════════
 *  Parse Door Mesh
 *═══════════════════════════════════════════════════════════════════*/
bool parse_door_mesh_dec(const u8* dec_data, size_t dec_size, u32 base_addr, Mesh& mesh)
{
    if (dec_size < 16) return false;

    u32 p0 = rd_u32(dec_data) - base_addr;
    u32 p1 = rd_u32(dec_data + 4) - base_addr;
    u32 counts = rd_u32(dec_data + 8);
    u16 n_tris  = (u16)(counts & 0xFFFF);
    u16 n_quads = (u16)((counts >> 16) & 0xFFFF);

    Array<MeshVert> verts;
    Array<MeshTri>  tris;
    verts.reserve(1024);
    tris.reserve(1024);

    /* Triangles (40 bytes each) */
    for (int i = 0; i < n_tris; i++) {
        size_t o = p0 + (size_t)i * 40;
        if (o + 40 > dec_size) break;

        s16 vs[3][3]; u8 uvs[3][2]; bool valid = true;
        for (int vi = 0; vi < 3; vi++) {
            vs[vi][0] = rd_s16(dec_data + o + vi * 8);
            vs[vi][1] = rd_s16(dec_data + o + vi * 8 + 2);
            vs[vi][2] = rd_s16(dec_data + o + vi * 8 + 4);
            u16 uv_raw = rd_u16(dec_data + o + vi * 8 + 6);
            uvs[vi][0] = (u8)(uv_raw & 0xFF);
            uvs[vi][1] = (u8)((uv_raw >> 8) & 0xFF);
            for (int a = 0; a < 3; a++)
                if (vs[vi][a] > MAX_COORD || vs[vi][a] < -MAX_COORD)
                    valid = false;
        }
        if (!valid) continue;

        u16 tp = rd_u16(dec_data + o + 24);
        u16 cl = rd_u16(dec_data + o + 26);
        u8 cols[3][3];
        for (int ci = 0; ci < 3; ci++) {
            cols[ci][0] = dec_data[o + 28 + ci * 4];
            cols[ci][1] = dec_data[o + 28 + ci * 4 + 1];
            cols[ci][2] = dec_data[o + 28 + ci * 4 + 2];
        }
        u32 bi = (u32)verts.count;
        for (int vi = 0; vi < 3; vi++) {
            MeshVert mv = { -(f32)vs[vi][0], (f32)(-vs[vi][1]), (f32)vs[vi][2] };
            verts.push(mv);
        }
        MeshTri t = {};
        t.idx[0] = bi; t.idx[1] = bi+1; t.idx[2] = bi+2;
        t.color = avg_colors(cols, 3); t.tpage = tp; t.clut = cl;
        set_vcols(t, cols, 0, 1, 2);
        for (int vi = 0; vi < 3; vi++) {
            t.uv[vi][0] = uvs[vi][0]; t.uv[vi][1] = uvs[vi][1];
        }
        tris.push(t);
    }

    /* Quads (52 bytes each) */
    for (int i = 0; i < n_quads; i++) {
        size_t o = p1 + (size_t)i * 52;
        if (o + 52 > dec_size) break;

        s16 vs[4][3]; u8 uvs[4][2]; bool valid = true;
        for (int vi = 0; vi < 4; vi++) {
            vs[vi][0] = rd_s16(dec_data + o + vi * 8);
            vs[vi][1] = rd_s16(dec_data + o + vi * 8 + 2);
            vs[vi][2] = rd_s16(dec_data + o + vi * 8 + 4);
            u16 uv_raw = rd_u16(dec_data + o + vi * 8 + 6);
            uvs[vi][0] = (u8)(uv_raw & 0xFF);
            uvs[vi][1] = (u8)((uv_raw >> 8) & 0xFF);
            for (int a = 0; a < 3; a++)
                if (vs[vi][a] > MAX_COORD || vs[vi][a] < -MAX_COORD)
                    valid = false;
        }
        if (!valid) continue;

        u16 tp = rd_u16(dec_data + o + 32);
        u16 cl = rd_u16(dec_data + o + 34);
        u8 cols[4][3];
        for (int ci = 0; ci < 4; ci++) {
            cols[ci][0] = dec_data[o + 36 + ci * 4];
            cols[ci][1] = dec_data[o + 36 + ci * 4 + 1];
            cols[ci][2] = dec_data[o + 36 + ci * 4 + 2];
        }
        u32 bi = (u32)verts.count;
        RGBA8 c = avg_colors(cols, 4);
        for (int vi = 0; vi < 4; vi++) {
            MeshVert mv = { -(f32)vs[vi][0], (f32)(-vs[vi][1]), (f32)vs[vi][2] };
            verts.push(mv);
        }
        MeshTri t1 = {}, t2 = {};
        t1.idx[0]=bi; t1.idx[1]=bi+1; t1.idx[2]=bi+2; t1.color=c; t1.tpage=tp; t1.clut=cl;
        set_vcols(t1, cols, 0, 1, 2);
        t1.uv[0][0]=uvs[0][0]; t1.uv[0][1]=uvs[0][1];
        t1.uv[1][0]=uvs[1][0]; t1.uv[1][1]=uvs[1][1];
        t1.uv[2][0]=uvs[2][0]; t1.uv[2][1]=uvs[2][1];
        tris.push(t1);

        t2.idx[0]=bi+1; t2.idx[1]=bi+3; t2.idx[2]=bi+2; t2.color=c; t2.tpage=tp; t2.clut=cl;
        set_vcols(t2, cols, 1, 3, 2);
        t2.uv[0][0]=uvs[1][0]; t2.uv[0][1]=uvs[1][1];
        t2.uv[1][0]=uvs[3][0]; t2.uv[1][1]=uvs[3][1];
        t2.uv[2][0]=uvs[2][0]; t2.uv[2][1]=uvs[2][1];
        tris.push(t2);
    }

    mesh.alloc((int)verts.count, (int)tris.count);
    memcpy(mesh.verts, verts.items, verts.count * sizeof(MeshVert));
    memcpy(mesh.tris, tris.items, tris.count * sizeof(MeshTri));
    return mesh.tri_count > 0;
}

bool parse_door_mesh(const u8* data, size_t size, u32 base_addr, Mesh& mesh)
{
    Buffer dec;
    if (!lzss_decompress(data, size, dec)) return false;
    return parse_door_mesh_dec(dec.data, dec.size, base_addr, mesh);
}

/*═══════════════════════════════════════════════════════════════════
 *  Standalone Character EMD  (p*.dat / h*.dat entry 3)
 *
 *  Header:
 *    [0x00] u32 skeleton_ptr   (PSX addr → offset 0x58 typically)
 *    [0x04] u32 bone_count     (e.g. 19)
 *    [0x08] bone_table[bone_count+1] × 4 bytes  {u8 parent, u8 idx, u16 pad}
 *    ... skeleton data, animation data ...
 *
 *  Mesh part at end of blob (found by reverse scan):
 *    u32 tri_ptr               (PSX addr)
 *    u32 quad_ptr              (PSX addr)
 *    u16 tri_count
 *    u16 quad_count
 *
 *  Triangles: 48 bytes each
 *    3 × {s16 x, s16 y, s16 z, u8 U, u8 V}     = 24 bytes (vertices)
 *    3 × {s16 nx, s16 ny, s16 nz, u16 tag}      = 24 bytes (normals)
 *    n[0].tag = tpage, n[1].tag = clut, n[2].tag = 0
 *
 *  Quads: 64 bytes each
 *    4 × {s16 x, s16 y, s16 z, u8 U, u8 V}     = 32 bytes (vertices)
 *    4 × {s16 nx, s16 ny, s16 nz, u16 tag}      = 32 bytes (normals)
 *    n[0].tag = tpage, n[1].tag = clut, n[2..3].tag = 0
 *═══════════════════════════════════════════════════════════════════*/
bool is_standalone_emd_dec(const u8* dec, size_t dec_size, u32 base)
{
    if (dec_size < 0x100) return false;

    /* Check header: skeleton_ptr resolves within blob, bone_count is sane */
    u32 skel_ptr = rd_u32(dec);
    u32 skel_off = skel_ptr - base;
    if (skel_off < 0x20 || skel_off >= dec_size) return false;

    u32 bone_count = rd_u32(dec + 4);
    if (bone_count < 5 || bone_count > 60) return false;

    /* Bone table entries should fit before skeleton offset */
    size_t table_end = 8 + ((size_t)bone_count + 1) * 4;
    if (table_end > skel_off) return false;

    /* Verify bone table: each entry has lo_byte as parent (small value) */
    int valid_entries = 0;
    for (u32 i = 0; i < bone_count && i < 30; i++) {
        u8 parent = dec[8 + i * 4];
        if (parent < bone_count) valid_entries++;
    }
    if (valid_entries < (int)bone_count / 2) return false;

    /* Scan backwards from end for mesh part header: ptr, ptr, u16, u16
       where ptr2 - ptr1 == tri_count * 48 and data ends at dec_size */
    for (size_t scan = dec_size - 12; scan > dec_size / 2; scan -= 4) {
        u32 v0 = rd_u32(dec + scan);
        u32 v1 = rd_u32(dec + scan + 4);
        u32 o0 = v0 - base;
        u32 o1 = v1 - base;
        if (o0 >= dec_size || o1 >= dec_size || o1 <= o0) continue;
        u16 tc = rd_u16(dec + scan + 8);
        u16 qc = rd_u16(dec + scan + 10);
        if (tc > 200 || qc > 200 || tc + qc == 0) continue;
        if (o0 + (u32)tc * 48 != o1) continue;
        if (o1 + (u32)qc * 64 != (u32)dec_size) continue;
        return true;
    }
    return false;
}

bool parse_standalone_emd_mesh_dec(const u8* dec, size_t dec_size,
                                   u32 base, Mesh& mesh)
{
    if (dec_size < 0x100) return false;

    Array<MeshVert> verts;
    Array<MeshTri>  tris;
    verts.reserve(2048);
    tris.reserve(2048);

    /* Find mesh part header by reverse scan */
    size_t hdr_off = 0;
    u32 tri_off = 0, quad_off = 0;
    u16 tri_count = 0, quad_count = 0;
    bool found = false;

    for (size_t scan = dec_size - 12; scan > dec_size / 2; scan -= 4) {
        u32 v0 = rd_u32(dec + scan);
        u32 v1 = rd_u32(dec + scan + 4);
        u32 o0 = v0 - base;
        u32 o1 = v1 - base;
        if (o0 >= dec_size || o1 >= dec_size || o1 <= o0) continue;
        u16 tc = rd_u16(dec + scan + 8);
        u16 qc = rd_u16(dec + scan + 10);
        if (tc > 200 || qc > 200 || tc + qc == 0) continue;
        if (o0 + (u32)tc * 48 != o1) continue;
        if (o1 + (u32)qc * 64 != (u32)dec_size) continue;
        hdr_off = scan;
        tri_off = o0; quad_off = o1;
        tri_count = tc; quad_count = qc;
        found = true;
        break;
    }
    if (!found) return false;

    /* Parse 48-byte triangles:
       [0:24]  3 verts × 8  {s16 x, s16 y, s16 z, u8 U, u8 V}
       [24:32] n0 {s16, s16, s16, u16 tpage}
       [32:40] n1 {s16, s16, s16, u16 clut}
       [40:48] n2 {s16, s16, s16, u16 pad} */
    for (int i = 0; i < (int)tri_count; i++) {
        size_t o = (size_t)tri_off + (size_t)i * 48;
        if (o + 48 > dec_size) break;

        s16 vs[3][3];
        u8  uvs[3][2];
        bool valid = true;
        for (int vi = 0; vi < 3; vi++) {
            vs[vi][0] = rd_s16(dec + o + vi * 8);
            vs[vi][1] = rd_s16(dec + o + vi * 8 + 2);
            vs[vi][2] = rd_s16(dec + o + vi * 8 + 4);
            u16 uv_raw = rd_u16(dec + o + vi * 8 + 6);
            uvs[vi][0] = (u8)(uv_raw & 0xFF);
            uvs[vi][1] = (u8)((uv_raw >> 8) & 0xFF);
            for (int a = 0; a < 3; a++)
                if (vs[vi][a] > MAX_COORD || vs[vi][a] < -MAX_COORD)
                    valid = false;
        }
        if (!valid) continue;

        u16 tp = rd_u16(dec + o + 24 + 6);  /* n[0].tag = tpage */
        u16 cl = rd_u16(dec + o + 32 + 6);  /* n[1].tag = clut  */

        /* Read per-vertex normals (s16 × 3 at offsets 24, 32, 40) */
        s16 ns[3][3];
        for (int ni = 0; ni < 3; ni++) {
            ns[ni][0] = rd_s16(dec + o + 24 + ni * 8);
            ns[ni][1] = rd_s16(dec + o + 24 + ni * 8 + 2);
            ns[ni][2] = rd_s16(dec + o + 24 + ni * 8 + 4);
        }

        u32 bi = (u32)verts.count;
        for (int vi = 0; vi < 3; vi++) {
            MeshVert mv = {};
            mv.x = -(f32)vs[vi][0];
            mv.y = -(f32)vs[vi][1];
            mv.z =  (f32)vs[vi][2];
            mv.nx = -(f32)ns[vi][0];
            mv.ny = -(f32)ns[vi][1];
            mv.nz =  (f32)ns[vi][2];
            verts.push(mv);
        }
        MeshTri t = {};
        t.idx[0] = bi; t.idx[1] = bi+2; t.idx[2] = bi+1;
        t.color.r = 128; t.color.g = 128; t.color.b = 128; t.color.a = 255;
        t.tpage = tp; t.clut = cl;
        t.uv[0][0] = uvs[0][0]; t.uv[0][1] = uvs[0][1];
        t.uv[1][0] = uvs[2][0]; t.uv[1][1] = uvs[2][1];
        t.uv[2][0] = uvs[1][0]; t.uv[2][1] = uvs[1][1];
        tris.push(t);
    }

    /* Parse 64-byte quads:
       [0:32]  4 verts × 8
       [32:40] n0 {.., u16 tpage}
       [40:48] n1 {.., u16 clut}
       [48:64] n2, n3 */
    for (int i = 0; i < (int)quad_count; i++) {
        size_t o = (size_t)quad_off + (size_t)i * 64;
        if (o + 64 > dec_size) break;

        s16 vs[4][3];
        u8  uvs[4][2];
        bool valid = true;
        for (int vi = 0; vi < 4; vi++) {
            vs[vi][0] = rd_s16(dec + o + vi * 8);
            vs[vi][1] = rd_s16(dec + o + vi * 8 + 2);
            vs[vi][2] = rd_s16(dec + o + vi * 8 + 4);
            u16 uv_raw = rd_u16(dec + o + vi * 8 + 6);
            uvs[vi][0] = (u8)(uv_raw & 0xFF);
            uvs[vi][1] = (u8)((uv_raw >> 8) & 0xFF);
            for (int a = 0; a < 3; a++)
                if (vs[vi][a] > MAX_COORD || vs[vi][a] < -MAX_COORD)
                    valid = false;
        }
        if (!valid) continue;

        u16 tp = rd_u16(dec + o + 32 + 6);  /* n[0].tag = tpage */
        u16 cl = rd_u16(dec + o + 40 + 6);  /* n[1].tag = clut  */

        /* Read per-vertex normals (s16 × 3 at offsets 32, 40, 48, 56) */
        s16 ns[4][3];
        for (int ni = 0; ni < 4; ni++) {
            ns[ni][0] = rd_s16(dec + o + 32 + ni * 8);
            ns[ni][1] = rd_s16(dec + o + 32 + ni * 8 + 2);
            ns[ni][2] = rd_s16(dec + o + 32 + ni * 8 + 4);
        }

        RGBA8 c;
        c.r = 128; c.g = 128; c.b = 128; c.a = 255;

        u32 bi = (u32)verts.count;
        for (int vi = 0; vi < 4; vi++) {
            MeshVert mv = {};
            mv.x = -(f32)vs[vi][0];
            mv.y = -(f32)vs[vi][1];
            mv.z =  (f32)vs[vi][2];
            mv.nx = -(f32)ns[vi][0];
            mv.ny = -(f32)ns[vi][1];
            mv.nz =  (f32)ns[vi][2];
            verts.push(mv);
        }

        MeshTri t1 = {};
        t1.idx[0] = bi; t1.idx[1] = bi+2; t1.idx[2] = bi+1;
        t1.color = c; t1.tpage = tp; t1.clut = cl;
        t1.uv[0][0] = uvs[0][0]; t1.uv[0][1] = uvs[0][1];
        t1.uv[1][0] = uvs[2][0]; t1.uv[1][1] = uvs[2][1];
        t1.uv[2][0] = uvs[1][0]; t1.uv[2][1] = uvs[1][1];
        tris.push(t1);

        MeshTri t2 = {};
        t2.idx[0] = bi+1; t2.idx[1] = bi+2; t2.idx[2] = bi+3;
        t2.color = c; t2.tpage = tp; t2.clut = cl;
        t2.uv[0][0] = uvs[1][0]; t2.uv[0][1] = uvs[1][1];
        t2.uv[1][0] = uvs[2][0]; t2.uv[1][1] = uvs[2][1];
        t2.uv[2][0] = uvs[3][0]; t2.uv[2][1] = uvs[3][1];
        tris.push(t2);
    }

    if (tris.count == 0) return false;

    /* Copy to output mesh */
    mesh.vert_count = verts.count;
    mesh.tri_count  = tris.count;
    mesh.verts = new MeshVert[verts.count];
    mesh.tris  = new MeshTri[tris.count];
    memcpy(mesh.verts, verts.items, sizeof(MeshVert) * verts.count);
    memcpy(mesh.tris,  tris.items,  sizeof(MeshTri)  * tris.count);
    mesh_compute_smooth_normals(mesh);
    return true;
}

/*═══════════════════════════════════════════════════════════════════
 *  Entry type detection
 *═══════════════════════════════════════════════════════════════════*/
bool is_door_mesh_entry_dec(const u8* dec_data, size_t dec_size, u16 ey)
{
    if (dec_size < 16 || dec_size > 10000) return false;

    u32 base = (u32)((ey & 0x7FFF) << 16) | 0x80000000u;
    u32 p0 = rd_u32(dec_data);
    u32 p1 = rd_u32(dec_data + 4);

    if ((p0 >> 24) != 0x80 || (p1 >> 24) != 0x80) return false;
    if (p0 == p1) return false;

    u32 off0 = p0 - base, off1 = p1 - base;
    if (off0 < 12 || off0 >= dec_size || off1 < 12 || off1 > dec_size) return false;

    u32 counts = rd_u32(dec_data + 8);
    u16 nt = (u16)(counts & 0xFFFF);
    u16 nq = (u16)((counts >> 16) & 0xFFFF);
    if (nt > 200 || nq > 200) return false;
    if (off0 + nt * 40 > dec_size + 4) return false;
    if (off1 + nq * 52 > dec_size + 4) return false;
    return (nt + nq) > 0;
}

bool is_door_mesh_entry(const u8* data, size_t size, u16 ey, u16 ew, u16 eh)
{
    (void)ew; (void)eh;
    if (!(ey & 0x8000)) return false;
    Buffer dec;
    if (!lzss_decompress(data, size, dec)) return false;
    return is_door_mesh_entry_dec(dec.data, dec.size, ey);
}

bool is_emd_entry(const u8* data, size_t size, u16 ey, u16 ex)
{
    RdtLayout layout;
    if (!parse_rdt_layout_from_entry(data, size, ey, ex, layout))
        return false;
    return layout.emd_count > 0;
}

/*═══════════════════════════════════════════════════════════════════
 *  Find ALL EMD headers in a decompressed LZSS0 blob.
 *  Returns count found (up to max_out).
 *═══════════════════════════════════════════════════════════════════*/
int find_emd_headers(const u8* data, size_t size, u16 ey, u16 ex,
                     size_t* out_offsets, int max_out)
{
    RdtLayout layout;
    if (!parse_rdt_layout_from_entry(data, size, ey, ex, layout))
        return 0;

    int count = layout.emd_count;
    if (count > max_out) count = max_out;
    for (int i = 0; i < count; i++)
        out_offsets[i] = layout.emds[i].offset;
    return count;
}

/*═══════════════════════════════════════════════════════════════════
 *  EMD Character Model Parser
 *═══════════════════════════════════════════════════════════════════*/
bool parse_emd_model_dec(const u8* dec_data, size_t dec_size,
                        u16 ey, u16 ex, EmdModel& model,
                        int hint_offset)
{
    model.valid = false;

    if (dec_size < 200) return false;

    u32 base = ((u32)(ey & 0x7FFF) << 16) | (u32)ex | 0x80000000u;

    /* Find EMD header */
    size_t hdr_off = 0;
    int n_verts = 0, total_tris = 0, total_quads = 0, n_parts = 0;
    (void)total_tris; (void)total_quads;
    u32 targets[4] = {0};
    bool found = false;

    /* If hint_offset given, start scan there (for multi-EMD entries) */
    size_t scan_start = (hint_offset >= 0) ? (size_t)hint_offset : 0;

    for (size_t soff = scan_start; soff + 24 <= dec_size; soff += 4) {
        u32 ptrs[4];
        for (int i = 0; i < 4; i++)
            ptrs[i] = rd_u32(dec_data + soff + i * 4);
        bool all_valid = true;
        for (int i = 0; i < 4; i++)
            if (ptrs[i] < base || ptrs[i] >= base + dec_size)
                { all_valid = false; break; }
        if (!all_valid) continue;

        for (int i = 0; i < 4; i++) targets[i] = ptrs[i] - base;
        if (!(targets[0] < targets[1] && targets[1] < targets[2] && targets[2] < targets[3]))
            continue;

        int nv = (int)(targets[1] - targets[0]) / 8;
        int nn = (int)(targets[2] - targets[1]) / 8;
        if (nv != nn || nv < 10 || nv > 5000) continue;

        if (soff + 22 > dec_size) continue;
        u16 tt = rd_u16(dec_data + soff + 16);
        u16 tq = rd_u16(dec_data + soff + 18);
        u16 np = rd_u16(dec_data + soff + 20);
        if (tt < 1 || tt > 5000 || tq > 5000) continue;
        if (np < 1 || np > 50) continue;
        if (targets[2] + (u32)tt * 16 != targets[3]) continue;
        size_t he = soff + 24 + (size_t)np * 20;
        if (he > targets[0]) continue;

        hdr_off = soff; n_verts = nv;
        total_tris = tt; total_quads = tq; n_parts = np;
        found = true;
        break;
    }
    if (!found) return false;

    /* Read body parts */
    struct Part {
        s16 bx, by, bz; int parent;
        u32 tri_ptr, quad_ptr; int nt, nq;
    };
    Part parts[50];
    for (int pi = 0; pi < n_parts; pi++) {
        size_t poff = hdr_off + 24 + (size_t)pi * 20;
        parts[pi].bx = rd_s16(dec_data + poff);
        parts[pi].by = rd_s16(dec_data + poff + 2);
        parts[pi].bz = rd_s16(dec_data + poff + 4);
        parts[pi].parent = dec_data[poff + 6];
        if (parts[pi].parent == 0xFF) parts[pi].parent = -1;
        parts[pi].tri_ptr  = rd_u32(dec_data + poff + 8) - base;
        parts[pi].quad_ptr = rd_u32(dec_data + poff + 12) - base;
        parts[pi].nt = rd_u16(dec_data + poff + 16);
        parts[pi].nq = rd_u16(dec_data + poff + 18);
    }

    /* Chain bone positions */
    f32 abs_pos[50][3];
    for (int pi = 0; pi < n_parts; pi++) {
        f32 px = (f32)parts[pi].bx, py = (f32)parts[pi].by, pz = (f32)parts[pi].bz;
        int par = parts[pi].parent;
        if (par == -1 || par >= n_parts) {
            abs_pos[pi][0] = px; abs_pos[pi][1] = py; abs_pos[pi][2] = pz;
        } else {
            abs_pos[pi][0] = abs_pos[par][0] + px;
            abs_pos[pi][1] = abs_pos[par][1] + py;
            abs_pos[pi][2] = abs_pos[par][2] + pz;
        }
    }

    /* Read vertex pool */
    model.pool = (EmdModel::PoolVert*)calloc(n_verts, sizeof(EmdModel::PoolVert));
    model.pool_count = n_verts;
    for (int vi = 0; vi < n_verts; vi++) {
        size_t voff = targets[0] + (size_t)vi * 8;
        model.pool[vi].x = rd_s16(dec_data + voff);
        model.pool[vi].y = rd_s16(dec_data + voff + 2);
        model.pool[vi].z = rd_s16(dec_data + voff + 4);
        model.pool[vi].bone_idx = rd_u16(dec_data + voff + 6);
        if (model.pool[vi].bone_idx >= (u16)n_parts)
            model.pool[vi].bone_idx = 0;
    }

    /* Transform vertices to world space */
    f32* V_world = (f32*)calloc(n_verts * 3, sizeof(f32));
    f32* N_world = (f32*)calloc(n_verts * 3, sizeof(f32));  /* per-vertex normals */
    for (int vi = 0; vi < n_verts; vi++) {
        int bi = model.pool[vi].bone_idx;
        V_world[vi*3]   = (f32)model.pool[vi].x + abs_pos[bi][0];
        V_world[vi*3+1] = -((f32)model.pool[vi].y + abs_pos[bi][1]);
        V_world[vi*3+2] = -((f32)model.pool[vi].z + abs_pos[bi][2]);
        /* Read normal from pool at targets[1] (same stride/count as vertex pool) */
        size_t noff = targets[1] + (size_t)vi * 8;
        if (noff + 6 <= dec_size) {
            f32 rnx = (f32)rd_s16(dec_data + noff);
            f32 rny = (f32)rd_s16(dec_data + noff + 2);
            f32 rnz = (f32)rd_s16(dec_data + noff + 4);
            /* Apply same coordinate transform as vertices (negate Y, negate Z) */
            N_world[vi*3]   = rnx;
            N_world[vi*3+1] = -rny;
            N_world[vi*3+2] = -rnz;
        }
    }

    /* Per-part colors */
    static const u8 pcols[][3] = {
        {200,100,100},{100,200,100},{100,100,200},{200,200,100},
        {200,100,200},{100,200,200},{180,180,100},{255,150,80},
        {150,80,255},{80,255,150},{255,80,150},{150,255,80},
        {80,150,255},{200,150,150},{150,200,150}
    };

    /* Detect mirrored parts */
    int mirror_map[50];
    for (int i = 0; i < 50; i++) mirror_map[i] = -1;
    for (int pi = 0; pi < n_parts; pi++) {
        if (parts[pi].nt + parts[pi].nq > 0) continue;
        for (int pj = 0; pj < n_parts; pj++) {
            if (pj == pi || parts[pj].nt + parts[pj].nq == 0) continue;
            f32 ax = abs_pos[pi][0], ay = abs_pos[pi][1], az = abs_pos[pi][2];
            f32 bx2 = abs_pos[pj][0], by2 = abs_pos[pj][1], bz2 = abs_pos[pj][2];
            if (fabsf(ax + bx2) <= 1 && fabsf(ay - by2) <= 1 && fabsf(az - bz2) <= 1) {
                mirror_map[pi] = pj;
                break;
            }
        }
    }

    /* Emit faces */
    Array<MeshVert> out_v; out_v.reserve(8192);
    Array<MeshTri>  out_t; out_t.reserve(8192);
    Array<EmdModel::VertSrc> out_vs; out_vs.reserve(8192);

    for (int pi = 0; pi < n_parts; pi++) {
        RGBA8 color = RGBA8(pcols[pi%15][0], pcols[pi%15][1], pcols[pi%15][2], 255);
        bool is_mirror = (mirror_map[pi] >= 0);
        Part& src = is_mirror ? parts[mirror_map[pi]] : parts[pi];
        if (src.nt + src.nq == 0) continue;

        /* Tri faces (16 bytes each) */
        for (int fi = 0; fi < src.nt; fi++) {
            size_t foff = src.tri_ptr + (size_t)fi * 16;
            if (foff + 16 > dec_size) break;
            u16 i0 = rd_u16(dec_data + foff);
            u16 i1 = rd_u16(dec_data + foff + 2);
            u16 i2 = rd_u16(dec_data + foff + 4);
            if (i0 >= n_verts || i1 >= n_verts || i2 >= n_verts) continue;

            /* EMD tri (16 bytes):
               +0: idx0  +2: idx1  +4: idx2
               +6: u0,v0  +8: u1,v1  +10: CLUT  +12: u2,v2  +14: TPAGE
               UV-to-vertex mapping: uv0→idx1, uv1→idx2, uv2→idx0 */
            u8 uv0[2] = { dec_data[foff+6],  dec_data[foff+7]  };
            u8 uv1[2] = { dec_data[foff+8],  dec_data[foff+9]  };
            u8 uv2[2] = { dec_data[foff+12], dec_data[foff+13] };
            u16 tp = rd_u16(dec_data + foff + 14);
            u16 cl = rd_u16(dec_data + foff + 10);

            u32 bi = (u32)out_v.count;
            u16 idxs[3] = {i0, i1, i2};
            for (int vi = 0; vi < 3; vi++) {
                MeshVert mv = {};
                EmdModel::VertSrc vs;
                vs.pool_idx = idxs[vi];
                vs.is_mirror = is_mirror;
                vs.dest_bone = pi;
                if (is_mirror) {
                    mv.x = V_world[idxs[vi]*3];   /* mirror+coord negates cancel */
                    mv.nx = N_world[idxs[vi]*3];
                } else {
                    mv.x = -V_world[idxs[vi]*3];  /* coord system X negate */
                    mv.nx = -N_world[idxs[vi]*3];
                }
                mv.y = V_world[idxs[vi]*3+1];
                mv.z = V_world[idxs[vi]*3+2];
                mv.ny = N_world[idxs[vi]*3+1];
                mv.nz = N_world[idxs[vi]*3+2];
                out_v.push(mv);
                out_vs.push(vs);
            }

            MeshTri t = {};
            t.color = color; t.tpage = tp; t.clut = cl;
            if (is_mirror) {
                t.idx[0]=bi; t.idx[1]=bi+2; t.idx[2]=bi+1;
                t.uv[0][0]=uv1[0]; t.uv[0][1]=uv1[1];
                t.uv[1][0]=uv0[0]; t.uv[1][1]=uv0[1];
                t.uv[2][0]=uv2[0]; t.uv[2][1]=uv2[1];
            } else {
                t.idx[0]=bi; t.idx[1]=bi+1; t.idx[2]=bi+2;
                t.uv[0][0]=uv1[0]; t.uv[0][1]=uv1[1];
                t.uv[1][0]=uv2[0]; t.uv[1][1]=uv2[1];
                t.uv[2][0]=uv0[0]; t.uv[2][1]=uv0[1];
            }
            out_t.push(t);
        }

        /* Quad faces (20 bytes each) */
        for (int fi = 0; fi < src.nq; fi++) {
            size_t foff = src.quad_ptr + (size_t)fi * 20;
            if (foff + 20 > dec_size) break;
            u16 i0 = rd_u16(dec_data + foff);
            u16 i1 = rd_u16(dec_data + foff + 2);
            u16 i2 = rd_u16(dec_data + foff + 4);
            u16 i3 = rd_u16(dec_data + foff + 6);
            if (i0>=n_verts || i1>=n_verts || i2>=n_verts || i3>=n_verts) continue;

            u8 uv0[2]={dec_data[foff+8], dec_data[foff+9]};
            u8 uv1[2]={dec_data[foff+12],dec_data[foff+13]};
            u8 uv2[2]={dec_data[foff+16],dec_data[foff+17]};
            u8 uv3[2]={dec_data[foff+18],dec_data[foff+19]};
            u16 tp = rd_u16(dec_data + foff + 14);
            u16 cl = rd_u16(dec_data + foff + 10);

            u32 bi = (u32)out_v.count;
            u16 idxs[4] = {i0, i1, i2, i3};
            for (int vi = 0; vi < 4; vi++) {
                MeshVert mv = {};
                EmdModel::VertSrc vs;
                vs.pool_idx = idxs[vi];
                vs.is_mirror = is_mirror;
                vs.dest_bone = pi;
                if (is_mirror) {
                    mv.x = V_world[idxs[vi]*3];   /* mirror+coord negates cancel */
                    mv.nx = N_world[idxs[vi]*3];
                } else {
                    mv.x = -V_world[idxs[vi]*3];  /* coord system X negate */
                    mv.nx = -N_world[idxs[vi]*3];
                }
                mv.y = V_world[idxs[vi]*3+1];
                mv.z = V_world[idxs[vi]*3+2];
                mv.ny = N_world[idxs[vi]*3+1];
                mv.nz = N_world[idxs[vi]*3+2];
                out_v.push(mv);
                out_vs.push(vs);
            }

            MeshTri t1 = {}, t2 = {};
            t1.color = color; t1.tpage = tp; t1.clut = cl;
            t2.color = color; t2.tpage = tp; t2.clut = cl;


            if (is_mirror) {
                /* Mirror quad: reversed winding, PS1 strip diagonal (v1-v2) */
                t1.idx[0]=bi;   t1.idx[1]=bi+2; t1.idx[2]=bi+1;
                t1.uv[0][0]=uv0[0]; t1.uv[0][1]=uv0[1];
                t1.uv[1][0]=uv2[0]; t1.uv[1][1]=uv2[1];
                t1.uv[2][0]=uv1[0]; t1.uv[2][1]=uv1[1];

                t2.idx[0]=bi+1; t2.idx[1]=bi+2; t2.idx[2]=bi+3;
                t2.uv[0][0]=uv1[0]; t2.uv[0][1]=uv1[1];
                t2.uv[1][0]=uv2[0]; t2.uv[1][1]=uv2[1];
                t2.uv[2][0]=uv3[0]; t2.uv[2][1]=uv3[1];
            } else {
                /* Standard PS1 strip: T1=(v0,v1,v2) T2=(v1,v3,v2) */
                t1.idx[0]=bi; t1.idx[1]=bi+1; t1.idx[2]=bi+2;
                t1.uv[0][0]=uv0[0]; t1.uv[0][1]=uv0[1];
                t1.uv[1][0]=uv1[0]; t1.uv[1][1]=uv1[1];
                t1.uv[2][0]=uv2[0]; t1.uv[2][1]=uv2[1];

                t2.idx[0]=bi+1; t2.idx[1]=bi+3; t2.idx[2]=bi+2;
                t2.uv[0][0]=uv1[0]; t2.uv[0][1]=uv1[1];
                t2.uv[1][0]=uv3[0]; t2.uv[1][1]=uv3[1];
                t2.uv[2][0]=uv2[0]; t2.uv[2][1]=uv2[1];
            }
            out_t.push(t1);
            out_t.push(t2);
        }
    }

    free(V_world);
    free(N_world);

    /* Store raw decompressed data for animation playback */
    model.raw_dec = (u8*)malloc(dec_size);
    if (model.raw_dec) {
        memcpy(model.raw_dec, dec_data, dec_size);
        model.raw_dec_size = dec_size;
    }
    model.base_addr = base;
    model.norm_pool_off = targets[1];  /* offset of normal pool for animation */

    /* Store part info for FK chain */
    model.parts_info = (EmdModel::PartInfo*)calloc(n_parts, sizeof(EmdModel::PartInfo));
    model.parts_count = n_parts;
    for (int pi = 0; pi < n_parts; pi++) {
        model.parts_info[pi].bx = parts[pi].bx;
        model.parts_info[pi].by = parts[pi].by;
        model.parts_info[pi].bz = parts[pi].bz;
        model.parts_info[pi].parent = parts[pi].parent;
    }

    /* Store vert_src for animation (per output vertex -> pool vertex mapping) */
    model.vert_src = (EmdModel::VertSrc*)calloc(out_vs.count, sizeof(EmdModel::VertSrc));
    model.vert_src_count = (int)out_vs.count;
    if (model.vert_src && out_vs.count > 0)
        memcpy(model.vert_src, out_vs.items, out_vs.count * sizeof(EmdModel::VertSrc));

    /* Build skeleton */
    model.skeleton.bone_count = n_parts;
    for (int pi = 0; pi < n_parts; pi++) {
        EmdBone& bone = model.skeleton.bones[pi];
        bone.bx = parts[pi].bx;
        bone.by = parts[pi].by;
        bone.bz = parts[pi].bz;
        bone.parent = (u8)((parts[pi].parent == -1) ? 0xFF : parts[pi].parent);
        model.skeleton.abs_pos[pi][0] = -abs_pos[pi][0];
        model.skeleton.abs_pos[pi][1] = -abs_pos[pi][1];
        model.skeleton.abs_pos[pi][2] = -abs_pos[pi][2];
    }

    /* Copy mesh */
    model.mesh.alloc((int)out_v.count, (int)out_t.count);
    memcpy(model.mesh.verts, out_v.items, out_v.count * sizeof(MeshVert));
    memcpy(model.mesh.tris, out_t.items, out_t.count * sizeof(MeshTri));
    if (model.mesh.tri_count > 0) mesh_compute_smooth_normals(model.mesh);
    model.valid = true;
    extract_emd_anims(model);
    return true;
}


bool parse_emd_model(const u8* entry_data, size_t entry_size,
                     u16 ey, u16 ex, EmdModel& model,
                     int hint_offset)
{
    Buffer dec;
    if (!lzss_decompress(entry_data, entry_size, dec)) return false;
    return parse_emd_model_dec(dec.data, dec.size, ey, ex, model, hint_offset);
}

/*═══════════════════════════════════════════════════════════════════
 *  EMD Animation Extraction
 *═══════════════════════════════════════════════════════════════════*/
/* True when nframes frames of fs bytes at target fit in the decompressed
   buffer and every frame header passes: bytes 0-5 are s16 root
   position/rotation values that should be bounded. */
static bool emd_clip_frames_ok(const u8* dec, size_t dsz, size_t target,
                               int fs, u32 nframes)
{
    /* All frames must fit within the decompressed buffer */
    if (target + (size_t)nframes * (size_t)fs > dsz) return false;
    for (u32 fi = 0; fi < nframes; fi++) {
        size_t foff = target + (size_t)fi * (size_t)fs;
        s16 h0 = rd_s16(dec + foff);
        s16 h1 = rd_s16(dec + foff + 2);
        s16 h2 = rd_s16(dec + foff + 4);
        if (h0 < -4000 || h0 > 4000 ||
            h1 < -4000 || h1 > 4000 ||
            h2 < -4000 || h2 > 4000)
            return false;
    }
    return true;
}

void extract_emd_anims(EmdModel& model)
{
    if (!model.raw_dec || model.raw_dec_size < 12) return;
    const u8* dec = model.raw_dec;
    size_t dsz = model.raw_dec_size;
    u32 base = model.base_addr;

    /* ── Build candidate frame sizes ──────────────────────────────
       The animation bitstream encodes 12-bit rotation triplets for
       N bones, but N may be LESS than model.parts_count when mirror
       or empty bones are excluded.  We try every plausible bone count
       from 1..parts_count and collect the valid frame sizes.  The
       correct size is then determined per-clip by validating ALL
       frame headers. */
    int cand_fsizes[64];
    int n_cands = 0;
    for (int nb = 1; nb <= model.parts_count && n_cands < 64; nb++) {
        int rot_bytes = (nb * 36 + 7) / 8;
        int fs = 12 + rot_bytes;
        fs = (fs + 3) & ~3;
        /* Deduplicate (adjacent bone counts can round to same size) */
        if (n_cands > 0 && cand_fsizes[n_cands - 1] == fs) continue;
        cand_fsizes[n_cands++] = fs;
    }

    Array<EmdAnimClip> clips;
    clips.reserve(64);

    for (size_t scan = 0; scan + 12 <= dsz; scan += 4) {
        u32 ptr = rd_u32(dec + scan);
        if (ptr < base || ptr >= base + dsz) continue;
        u32 count = rd_u32(dec + scan + 4);
        if (count < 1 || count > 500) continue;

        u32 target = ptr - base;
        if (target != scan + 8 + ((size_t)count + 1) * 4) continue;

        /* The stored count is one less than the number of frames: the
           index table has count + 1 words, and in ST603's Therizinosaurus
           the next clip's head follows the last frame by exactly one more
           frame's worth of bytes, for all 48 gaps between its 49 clips.
           A few clips have no valid extra frame, with the next head right
           after count frames (a held pose of 4 frames in ST100); those
           keep count frames, as before.

           Try candidate frame sizes (largest first = most bones) and
           pick the largest where ALL frame headers pass validation. */
        int best_fs = 0;
        u32 nframes = count;
        for (int extra = 1; extra >= 0 && best_fs == 0; extra--) {
            nframes = count + (u32)extra;
            for (int ci = n_cands - 1; ci >= 0; ci--) {
                if (emd_clip_frames_ok(dec, dsz, target, cand_fsizes[ci], nframes)) {
                    best_fs = cand_fsizes[ci];
                    break;  /* take largest valid size */
                }
            }
        }
        if (best_fs == 0) continue;

        EmdAnimClip c;
        c.offset = (int)scan;
        c.frame_data = (int)target;
        c.frames = (int)nframes;
        c.frame_size = best_fs;
        clips.push(c);
    }

    if (clips.count > 0) {
        model.clips = (EmdAnimClip*)calloc(clips.count, sizeof(EmdAnimClip));
        model.clip_count = (int)clips.count;
        memcpy(model.clips, clips.items, clips.count * sizeof(EmdAnimClip));
        model.n_anims = model.clip_count;
    }
}

/*═══════════════════════════════════════════════════════════════════
 *  EMD Animation Frame Computation
 *
 *  Verified against game exe (sub_45ED3B, sub_459DB0, sub_5F3680):
 *   - Root bone rotation from frame header bytes 0-5 (s16 rx, ry, rz)
 *   - Packed 12-bit bitstream at byte 12+ for bones 1..N-1
 *   - Bitstream triplet order: rx, rz, ry (NOT rx, ry, rz)
 *   - Hierarchical FK with parent matrix accumulation
 *   - Matrix order: M = Rx * Ry * Rz (Z applied first)
 *═══════════════════════════════════════════════════════════════════*/

/* 3x3 matrix multiply: C = A * B */
static void mat3x3_mul(const f32* A, const f32* B, f32* C)
{
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            C[r*3+c] = A[r*3+0]*B[0*3+c]
                      + A[r*3+1]*B[1*3+c]
                      + A[r*3+2]*B[2*3+c];
}

/* Build Rx * Ry * Rz rotation matrix (matches game sub_5F3680) */
static void build_rot_matrix(f32 rx, f32 ry, f32 rz, f32* m)
{
    f32 cx = cosf(rx), sx = sinf(rx);
    f32 cy = cosf(ry), sy = sinf(ry);
    f32 cz = cosf(rz), sz = sinf(rz);
    m[0] = cy*cz;             m[1] = -cy*sz;             m[2] = sy;
    m[3] = cx*sz + sx*sy*cz;  m[4] = cx*cz - sx*sy*sz;   m[5] = -sx*cy;
    m[6] = sx*sz - cx*sy*cz;  m[7] = sx*cz + cx*sy*sz;   m[8] = cx*cy;
}

bool decode_emd_pose(const EmdModel& model, int clip_idx, int frame_idx,
                     f32 bone_pos[50][3], f32 bone_mat[50][9], f32 bone_rots[50][3])
{
    if (!model.valid || !model.raw_dec || !model.clips || !model.parts_info) return false;
    if (clip_idx < 0 || clip_idx >= model.clip_count) return false;
    const EmdAnimClip& clip = model.clips[clip_idx];
    if (clip.frames < 1 || clip.frame_data < 0) return false;
    if (frame_idx < 0) frame_idx = 0;
    if (frame_idx >= clip.frames) frame_idx = clip.frames - 1;

    const u8* dec = model.raw_dec;
    size_t dsz = model.raw_dec_size;
    int n_parts = model.parts_count;
    if (n_parts < 1 || n_parts > 50) return false;

    int fsize = clip.frame_size;
    if (fsize < 12) return false;

    size_t foff = (size_t)clip.frame_data + (size_t)frame_idx * (size_t)fsize;
    if (foff + (size_t)fsize > dsz) return false;

    const f32 TWO_PI = 6.2831853f;

    /* ─── Frame header bytes 0-5: entity world rotation (NOT bone rotation).
       Bytes 6-11: root velocity / speed metadata.
       These are written to entity+68/70/72 by sub_45E632 and are separate
       from per-bone rotation angles at bone+40/42/44.  Not used for FK. ─── */

    memset(bone_rots, 0, sizeof(f32) * 50 * 3);

    /* ─── Packed 12-bit rotation bitstream for N animated bones ───
       The frame_size stored per-clip was determined empirically during
       extract_emd_anims.  The number of bones encoded in the bitstream
       may be LESS than model.parts_count (mirror/empty bones excluded).
       Derive the actual encoded bone count from the rotation data size:
         rot_bytes = fsize - 12
         n_anim_bones = rot_bytes * 8 / 36  (each bone = 3 × 12 bits)
       Bones beyond n_anim_bones keep their bind-pose rotation (0,0,0). */
    size_t rot_start = foff + 12;
    int rot_bytes = fsize - 12;
    if (rot_bytes < 0) rot_bytes = 0;
    int n_anim_bones = (rot_bytes * 8) / 36;
    if (n_anim_bones > n_parts) n_anim_bones = n_parts;
    int max_words = (rot_bytes + 3) / 4;
    if (max_words > 64) max_words = 64;

    u32 words[64];
    memset(words, 0, sizeof(words));
    for (int wi = 0; wi < max_words; wi++) {
        size_t woff = rot_start + (size_t)wi * 4;
        words[wi] = (woff + 4 <= dsz) ? rd_u32(dec + woff) : 0;
    }

    int total_bits = rot_bytes * 8;
    int bit_offset = 0;
    for (int bi = 0; bi < n_anim_bones; bi++) {
        u32 vals[3] = {0, 0, 0};
        for (int ai = 0; ai < 3; ai++) {
            u32 val = 0;
            if (bit_offset + 12 <= total_bits) {
                int word_idx = bit_offset / 32;
                int bit_pos  = bit_offset % 32;
                if (word_idx < max_words) {
                    if (bit_pos <= 20) {
                        val = (words[word_idx] >> bit_pos) & 0xFFF;
                    } else {
                        int lo_bits = 32 - bit_pos;
                        u32 lo = (words[word_idx] >> bit_pos) & ((1u << lo_bits) - 1);
                        int hi_bits = 12 - lo_bits;
                        u32 hi = (word_idx + 1 < max_words) ?
                            (words[word_idx + 1] & ((1u << hi_bits) - 1)) : 0;
                        val = lo | (hi << lo_bits);
                    }
                }
            }
            vals[ai] = val;
            bit_offset += 12;
        }
        /* Bitstream order is rx, ry, rz (sequential, no swap).
           When the clip encodes fewer bones than the skeleton has parts
           (n_anim_bones < n_parts), the rotation values use the opposite
           angle convention: val represents -angle rather than +angle.
           Negate by computing (4096 - val) & 0xFFF. */
        for (int ai = 0; ai < 3; ai++) {
            u32 v = vals[ai];
            if (n_anim_bones < n_parts && v != 0)
                v = (4096 - v) & 0xFFF;
            bone_rots[bi][ai] = (f32)v * TWO_PI / 4096.0f;
        }
    }

    /* ─── Hierarchical FK (matches game sub_459DB0) ───
       Each bone:
        1. Build local rotation matrix from bone's angles
        2. Rotate bone offset by parent's accumulated matrix
        3. Bone position = parent position + rotated offset
        4. Accumulated matrix = local_mat * parent_mat             */

    for (int bi = 0; bi < n_parts; bi++) {
        int parent = model.parts_info[bi].parent;
        f32 bx = (f32)model.parts_info[bi].bx;
        f32 by = (f32)model.parts_info[bi].by;
        f32 bz = (f32)model.parts_info[bi].bz;

        f32 local_mat[9];
        build_rot_matrix(bone_rots[bi][0], bone_rots[bi][1],
                         bone_rots[bi][2], local_mat);

        if (parent < -1 || parent >= bi) return false;
        if (parent == -1) {
            bone_pos[bi][0] = bx;
            bone_pos[bi][1] = by;
            bone_pos[bi][2] = bz;
            memcpy(bone_mat[bi], local_mat, sizeof(f32) * 9);
        } else {
            /* Rotate bone offset by parent's accumulated matrix */
            f32* pm = bone_mat[parent];
            f32 rbx = pm[0]*bx + pm[1]*by + pm[2]*bz;
            f32 rby = pm[3]*bx + pm[4]*by + pm[5]*bz;
            f32 rbz = pm[6]*bx + pm[7]*by + pm[8]*bz;

            bone_pos[bi][0] = bone_pos[parent][0] + rbx;
            bone_pos[bi][1] = bone_pos[parent][1] + rby;
            bone_pos[bi][2] = bone_pos[parent][2] + rbz;

            /* Accumulated matrix = parent × local (matches game GTE: sub_5F4EF0) */
            mat3x3_mul(pm, local_mat, bone_mat[bi]);
        }
    }

    return true;
}

bool compute_anim_frame(EmdModel& model, int clip_idx, int frame_idx)
{
    f32 bone_pos[50][3], bone_mat[50][9], bone_rots[50][3];
    if (!decode_emd_pose(model, clip_idx, frame_idx, bone_pos, bone_mat, bone_rots)) return false;
    int n_parts = model.parts_count;

    /* ─── Transform vertices ─── */
    if (!model.mesh.verts || !model.vert_src || !model.pool) return false;
    for (int vi = 0; vi < model.mesh.vert_count && vi < model.vert_src_count; vi++) {
        const EmdModel::VertSrc& vs = model.vert_src[vi];
        if (vs.pool_idx < 0 || vs.pool_idx >= model.pool_count) continue;
        const EmdModel::PoolVert& pv = model.pool[vs.pool_idx];
        /* For mirrored parts, pool_idx points to the source part's vertex
           whose bone_idx is the source bone (e.g. left hand).  Use dest_bone
           which is the actual part index (e.g. right hand) so the mirror
           vertices follow the correct bone in the FK hierarchy.
           Local X must also be negated because in the mirror bone's local
           space the vertex is at (-vx, vy, vz). */
        int bidx = vs.is_mirror ? vs.dest_bone : (int)pv.bone_idx;
        if (bidx >= n_parts) bidx = 0;

        f32* wr = bone_mat[bidx];
        f32* wp = bone_pos[bidx];
        f32 lx = (f32)pv.x, ly = (f32)pv.y, lz = (f32)pv.z;
        if (vs.is_mirror) lx = -lx;

        f32 rx2 = wr[0]*lx + wr[1]*ly + wr[2]*lz;
        f32 ry2 = wr[3]*lx + wr[4]*ly + wr[5]*lz;
        f32 rz2 = wr[6]*lx + wr[7]*ly + wr[8]*lz;

        f32 wx = -(wp[0] + rx2);
        f32 wy = -(wp[1] + ry2);
        f32 wz = -(wp[2] + rz2);

        /* Guard against degenerate values */
        if (wx != wx || wy != wy || wz != wz) continue;
        if (wx > 100000.0f || wx < -100000.0f ||
            wy > 100000.0f || wy < -100000.0f ||
            wz > 100000.0f || wz < -100000.0f) continue;

        model.mesh.verts[vi].x = wx;
        model.mesh.verts[vi].y = wy;
        model.mesh.verts[vi].z = wz;

        /* Transform normal by bone rotation matrix (no translation) */
        if (vs.pool_idx < model.pool_count) {
            size_t noff = model.norm_pool_off + (size_t)vs.pool_idx * 8;
            if (noff + 6 <= model.raw_dec_size) {
                f32 lnx = (f32)rd_s16(model.raw_dec + noff);
                f32 lny = (f32)rd_s16(model.raw_dec + noff + 2);
                f32 lnz = (f32)rd_s16(model.raw_dec + noff + 4);
                if (vs.is_mirror) lnx = -lnx;
                f32 rnx = wr[0]*lnx + wr[1]*lny + wr[2]*lnz;
                f32 rny = wr[3]*lnx + wr[4]*lny + wr[5]*lnz;
                f32 rnz = wr[6]*lnx + wr[7]*lny + wr[8]*lnz;
                /* Apply same coordinate transform as vertices */
                model.mesh.verts[vi].nx = -rnx;
                model.mesh.verts[vi].ny = -rny;
                model.mesh.verts[vi].nz = -rnz;
            }
        }
    }

    /* Update skeleton abs_pos for bone visualization */
    for (int bi = 0; bi < n_parts; bi++) {
        model.skeleton.abs_pos[bi][0] = -bone_pos[bi][0];
        model.skeleton.abs_pos[bi][1] = -bone_pos[bi][1];
        model.skeleton.abs_pos[bi][2] = -bone_pos[bi][2];
    }

    return true;
}

/*═══════════════════════════════════════════════════════════════════
 *  MIPS Code Detection
 *═══════════════════════════════════════════════════════════════════*/
bool is_mips_code_dec(const u8* dec_data, size_t dec_size)
{
    if (dec_size < 64) return false;

    /* A room RDT starts with its table pointers (ptr[0..6]), all PSX RAM
       addresses; as code that would be seven LB instructions in a row.
       Without this, mesh data after the table that happens to look like
       branches marked 13 rooms (ST304, ST504, ST611, ...) as code. */
    int ram_ptrs = 0;
    for (int i = 0; i < 7; i++) {
        u32 p = rd_u32(dec_data + i * 4);
        if (p >= 0x80000000u && p < 0x80200000u) ram_ptrs++;
    }
    if (ram_ptrs == 7) return false;

    int real_mips = 0;
    int nops = 0;
    int total = 0;
    for (size_t i = 0; i < 32 && i * 4 + 4 <= dec_size; i++) {
        u32 instr = rd_u32(dec_data + i * 4);
        total++;
        if (instr == 0x00000000) { nops++; continue; }
        u16 hi = (u16)(instr >> 16);
        u8 op = (u8)(hi >> 10);
        (void)op;
        if (hi == 0x27BD || hi == 0xAFBF || hi == 0xAFB0 ||
            (hi & 0xFF00) == 0x3C00 ||
            (hi & 0xFC00) == 0x0C00 ||
            (hi & 0xFC00) == 0x1000 ||
            (hi & 0xFC00) == 0x1400 ||
            hi == 0x03E0 ||
            hi == 0x0800)
            real_mips++;
    }
    return real_mips >= 3;
}

bool is_mips_code(const u8* data, size_t size, u16 ey, u16 ex)
{
    (void)ex;
    if (!(ey & 0x8000)) return false;
    Buffer dec;
    if (!lzss_decompress(data, size, dec)) return false;
    return is_mips_code_dec(dec.data, dec.size);
}

/*═══════════════════════════════════════════════════════════════════
 *  Weapon Data Detection
 *
 *  Weapon LZSS0 blobs contain PS1 GPU display list packets
 *  (pre-built polygon commands) with a skeleton/bone header.
 *  Characteristics:
 *  - Starts with zero-padded header (24+ bytes of 0x00)
 *  - Contains sequential bone parent indices (small u16 values 9-18 etc)
 *  - Contains PS1 GPU polygon commands (0x20-0x3F command bytes)
 *  - No valid MIPS prologue instructions
 *  - No valid mesh section pointers (room/door/EMD style)
 *═══════════════════════════════════════════════════════════════════*/
bool is_weapon_data_dec(const u8* dec_data, size_t dec_size)
{
    if (dec_size < 256) return false;

    int leading_zeros = 0;
    for (size_t i = 0; i < 64 && i < dec_size; i++) {
        if (dec_data[i] == 0) leading_zeros++;
        else break;
    }
    if (leading_zeros < 16) return false;

    int gpu_cmds = 0;
    for (size_t i = 0; i < dec_size - 3; i += 4) {
        u8 cmd = dec_data[i + 3];
        if (cmd >= 0x20 && cmd <= 0x3F) gpu_cmds++;
    }

    return gpu_cmds >= 15;
}

bool is_weapon_data(const u8* data, size_t size, u16 ey, u16 ex)
{
    (void)ex;
    if (!(ey & 0x8000)) return false;
    Buffer dec;
    if (!lzss_decompress(data, size, dec)) return false;
    return is_weapon_data_dec(dec.data, dec.size);
}

/*═══════════════════════════════════════════════════════════════════
 *  RDT Scene Detection  -  room mesh + instanced sub-objects
 *
 *  Structure:
 *    [0x00]  Inline mesh sections (40-byte tris, 52-byte quads)
 *    [after] Instance table:
 *      Record 0 (8 bytes): vert_ptr, normal_ptr (shared vertex pool)
 *      Record N (20 bytes): tri_face_ptr, quad_face_ptr,
 *                           tri_count, quad_count,
 *                           pos_x, pos_y, pos_z, rotation
 *    [after] Vertex data, normal data, indexed face data
 *    [after] Metadata (collision, cameras, scripts, etc.)
 *═══════════════════════════════════════════════════════════════════*/
bool is_rdt_scene(const u8* data, size_t size, u16 ey, u16 ex)
{
    RdtLayout layout;
    if (!parse_rdt_layout_from_entry(data, size, ey, ex, layout))
        return false;
    /* RDT scene = has room mesh sections (with or without EMDs) */
    return layout.section_count > 0;
}

/*═══════════════════════════════════════════════════════════════════
 *  Parse RDT Scene  -  room mesh + instanced sub-objects
 *═══════════════════════════════════════════════════════════════════*/
bool parse_rdt_scene_dec(const u8* dec_data, size_t dec_size, u32 base_addr,
                        Mesh& mesh,
                        const u32* exclude_offsets, int n_exclude)
{
    if (dec_size < 64) return false;

    u32 base = base_addr;

    Array<MeshVert> verts;
    Array<MeshTri>  tris;
    verts.reserve(8192);
    tris.reserve(8192);

    /* ─── Phase 0: Extract SCD opcode 0x23 section transforms ───
     *
     * The RDT header has 7 PSX pointers at offset 0x00.
     * ptr[5] = SCD bytecode base (script data).
     * Within the scripts, opcode 0x23 (32 bytes) registers inline mesh
     * sections as room models with world-space transforms:
     *
     *   [0]  u8  opcode (0x23)
     *   [1]  u8  model_slot
     *   [8]  u32 section_ptr (PSX addr of section header in this blob)
     *   [12] s16 pos_x       → model table offset +32
     *   [14] s16 pos_y       → model table offset +34
     *   [16] s16 pos_z       → model table offset +36
     *   [18] s16 rot_x       → model table offset +40
     *   [20] s16 rot_y       → model table offset +42
     *   [22] s16 rot_z       → model table offset +44
     *
     * Sections with pos=(0,0,0) already have world-space vertex data.
     * Non-zero positions indicate origin-centered mesh that needs placement.
     */

    struct ScdTransform {
        u32 section_off;   /* file offset of section header */
        s16 px, py, pz;   /* world position */
        s16 rx, ry, rz;   /* rotation (4096 = 360 deg) */
        bool alternate;    /* true if conditional variant (same pos as another) */
    };
    ScdTransform scd_xforms[64];
    int n_scd_xforms = 0;

    /* Check RDT header validity */
    size_t mesh_start = 0;
    size_t mesh_end = dec_size;
    bool has_rdt_hdr = false;
    {
        u32 p0 = rd_u32(dec_data);
        u32 p0_off = p0 - base;
        if (p0_off > 0 && p0_off < dec_size && p0_off > 0x100) {
            has_rdt_hdr = true;
            size_t meta_min = dec_size;
            for (int si = 0; si < 7; si++) {
                if ((size_t)si * 4 + 4 > dec_size) break;
                u32 sp = rd_u32(dec_data + si * 4) - base;
                if (sp > 0x40 && sp < meta_min)
                    meta_min = sp;
            }
            mesh_start = 0x1C;
            mesh_end = meta_min;

            /* Read ptr[5] = SCD bytecode base */
            u32 scd_raw = rd_u32(dec_data + 5 * 4);
            u32 scd_off = scd_raw - base;
            if (scd_off < dec_size && scd_off > mesh_end) {
                /* SCD starts with a table of u32 offsets to script entry points.
                 * Walk each script using correct opcode sizes to avoid false positives.
                 * Duplicate section transforms are merged by section_off.
                 * Conditional variant meshes (same position, different section) are
                 * merged by position — only the first variant is kept. */
                u32 first_off = rd_u32(dec_data + scd_off);
                int n_scripts = 0;
                if (first_off >= 4 && first_off <= 0x1000 && (first_off & 3) == 0)
                    n_scripts = (int)(first_off / 4);
                if (n_scripts > 64) n_scripts = 64;

                /* Opcode 0x28 size by type */
                static const int op28sz[] = {48,40,32,36,44,32,32,52,32,32,32,32};
                /* Fixed opcode sizes (index = opcode).
                   0x04=endif(1B), 0x10/0x11=control(1B).
                   0x0A=return(0=terminal), 0x0C=jump(0=handled separately). */
                static const u8 opsz[0x70] = {
                    1, 4, 4, 4, 1, 4, 4, 4, 4, 8, 0, 4, 0, 4, 4, 4,
                    1, 1, 4, 4, 4, 8, 4, 4, 4, 1, 1, 1, 1, 1, 1, 1,
                   24, 8, 4,32, 8, 4, 4, 4, 0, 8, 8, 8, 4, 4,20, 4,
                    4, 4, 4, 4, 4, 8, 8, 8, 4, 4,12, 4, 8,12, 4, 8,
                   12, 4,20, 8, 8, 8, 8, 4, 4, 4, 4,12,32, 8, 8, 8,
                    8, 8, 4,12, 4,12,28, 8, 8,20, 8,44, 4, 4,20,12,
                    4, 4, 8, 8, 4, 4, 4, 8, 8, 4, 8, 4, 4, 4, 4, 4,
                };

                for (int si = 0; si < n_scripts; si++) {
                    if (scd_off + (size_t)si * 4 + 4 > dec_size) break;
                    u32 srel = rd_u32(dec_data + scd_off + si * 4);
                    size_t pc = scd_off + srel;
                    if (pc >= dec_size) continue;

                    int steps = 0;
                    while (pc < dec_size && steps < 2000) {
                        u8 op = dec_data[pc];

                        /* Relative jump: follow */
                        if (op == 0x0C) {
                            if (pc + 4 > dec_size) break;
                            s16 jump = rd_s16(dec_data + pc + 2);
                            size_t tgt = (size_t)((int)pc + (int)jump);
                            if (tgt >= dec_size || tgt < scd_off) break;
                            pc = tgt; steps++; continue;
                        }

                        /* Compute advance */
                        int adv = 0;
                        if (op == 0x00 || (op >= 0x19 && op <= 0x1F) || op >= 0x70)
                            adv = 1;
                        else if (op == 0x05 || op == 0x0F) adv = 4;
                        else if (op == 0x28 && pc + 3 <= dec_size)
                            adv = (dec_data[pc+2] < 12) ? op28sz[dec_data[pc+2]] : 32;
                        else if (op == 0x5A && pc + 4 <= dec_size)
                            adv = 4 + 8 * dec_data[pc+3];
                        else if (op < 0x70) adv = opsz[op];
                        if (adv <= 0) break;

                        /* Extract 0x23 section transforms */
                        if (op == 0x23 && pc + 32 <= dec_size) {
                            u8 slot = dec_data[pc + 1];
                            if (slot <= 36) {
                                u32 sec_ptr_raw = rd_u32(dec_data + pc + 8);
                                u32 sec_off;
                                if (sec_ptr_raw >= base && (sec_ptr_raw - base) < dec_size)
                                    sec_off = sec_ptr_raw - base;
                                else if (sec_ptr_raw < dec_size)
                                    sec_off = sec_ptr_raw;
                                else { pc += adv; steps++; continue; }

                                if (sec_off >= mesh_start && sec_off + 12 <= mesh_end) {
                                    u32 sp1 = rd_u32(dec_data + sec_off) - base;
                                    u32 sp2 = rd_u32(dec_data + sec_off + 4) - base;
                                    u16 sc1 = rd_u16(dec_data + sec_off + 8);
                                    u16 sc2 = rd_u16(dec_data + sec_off + 10);
                                    if (sp1 < dec_size && sp2 < dec_size && sc1 < 5000 && sc2 < 5000) {
                                        s16 px = rd_s16(dec_data + pc + 12);
                                        s16 py = rd_s16(dec_data + pc + 14);
                                        s16 pz = rd_s16(dec_data + pc + 16);
                                        s16 rx = rd_s16(dec_data + pc + 18);
                                        s16 ry = rd_s16(dec_data + pc + 20);
                                        s16 rz = rd_s16(dec_data + pc + 22);

                                        if (n_scd_xforms < 64) {
                                            /* Dedup by section_off */
                                            bool dup = false;
                                            for (int di = 0; di < n_scd_xforms; di++) {
                                                if (scd_xforms[di].section_off == sec_off) { dup = true; break; }
                                            }
                                            if (!dup) {
                                                ScdTransform& x = scd_xforms[n_scd_xforms++];
                                                x.section_off = (u32)sec_off;
                                                x.px = px; x.py = py; x.pz = pz;
                                                x.rx = rx; x.ry = ry; x.rz = rz;
                                                x.alternate = false;
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        /* Extract 0x28 type 4 door section pointers.
                           sub_426DFC positions the section at door zone center. */
                        if (op == 0x28 && pc + 3 <= dec_size && dec_data[pc + 2] == 4 && pc + 44 <= dec_size) {
                            u8 mdl_slot = dec_data[pc + 34];
                            if (mdl_slot != 0xFF) {
                                u32 dsec_raw = rd_u32(dec_data + pc + 36);
                                u32 dsec_off = dsec_raw - base;
                                if (dsec_off >= mesh_start && dsec_off + 12 <= mesh_end) {
                                    u32 dsp1 = rd_u32(dec_data + dsec_off) - base;
                                    u32 dsp2 = rd_u32(dec_data + dsec_off + 4) - base;
                                    u16 dsc1 = rd_u16(dec_data + dsec_off + 8);
                                    u16 dsc2 = rd_u16(dec_data + dsec_off + 10);
                                    if (dsp1 < dec_size && dsp2 < dec_size && dsc1 < 5000 && dsc2 < 5000) {
                                        s16 a0 = rd_s16(dec_data + pc + 4);
                                        s16 a1 = rd_s16(dec_data + pc + 6);
                                        s16 a4 = rd_s16(dec_data + pc + 12);
                                        s16 a5 = rd_s16(dec_data + pc + 14);
                                        s16 cx = a4 + (s16)((a0 - a4) / 2);
                                        s16 cz = a5 + (s16)((a1 - a5) / 2);
                                        if (n_scd_xforms < 64) {
                                            bool dup = false;
                                            for (int di = 0; di < n_scd_xforms; di++)
                                                if (scd_xforms[di].section_off == dsec_off) { dup = true; break; }
                                            if (!dup) {
                                                ScdTransform& x = scd_xforms[n_scd_xforms++];
                                                x.section_off = dsec_off;
                                                x.px = cx; x.py = 0; x.pz = cz;
                                                x.rx = 0; x.ry = 0; x.rz = 0;
                                                x.alternate = false;
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        pc += (size_t)adv;
                        steps++;
                    }
                }
            }
        }
    }

    /* ─── Phase 1: Parse inline room mesh sections ─── */
    size_t off = mesh_start;

    int max_sections = 200;
    while (off + 12 <= mesh_end && max_sections-- > 0) {
        u32 p1 = rd_u32(dec_data + off) - base;
        u32 p2 = rd_u32(dec_data + off + 4) - base;
        u16 c1 = rd_u16(dec_data + off + 8);
        u16 c2 = rd_u16(dec_data + off + 10);

        if (p1 >= dec_size || p2 >= dec_size) break;
        if (c1 > 5000 || c2 > 5000) break;
        size_t tri_end  = p1 + (size_t)c1 * 40;
        size_t quad_end = p2 + (size_t)c2 * 52;
        if (tri_end > dec_size || quad_end > dec_size) break;
        if (c1 == 0 && c2 == 0) { off += 12; continue; }

        /* Check if this section should be excluded (rendered via mesh_apply_xforms) */
        {
            bool skip = false;
            for (int ei = 0; ei < n_exclude; ei++)
                if ((u32)off == exclude_offsets[ei]) { skip = true; break; }
            if (skip) { off = quad_end; continue; }
        }

        /* Look up SCD transform for this section */
        f32 tx = 0, ty = 0, tz = 0;
        f32 cr = 1.0f, sr = 0.0f;
        bool has_rot = false;
        bool is_alt_section = false;
        for (int xi = 0; xi < n_scd_xforms; xi++) {
            if (scd_xforms[xi].section_off == (u32)off) {
                tx = (f32)scd_xforms[xi].px;
                ty = (f32)scd_xforms[xi].py;
                tz = (f32)scd_xforms[xi].pz;
                is_alt_section = scd_xforms[xi].alternate;
                if (scd_xforms[xi].ry != 0) {
                    f32 angle = (f32)scd_xforms[xi].ry * 6.2831853f / 4096.0f;
                    cr = cosf(angle);
                    sr = sinf(angle);
                    has_rot = true;
                }
                break;
            }
        }

        /* Parse inline triangles (40 bytes each) */
        for (int i = 0; i < (int)c1; i++) {
            size_t o = p1 + (size_t)i * 40;
            if (o + 40 > dec_size) break;

            s16 vs[3][3];
            u8  uvs[3][2];
            for (int vi = 0; vi < 3; vi++) {
                vs[vi][0] = rd_s16(dec_data + o + vi * 8);
                vs[vi][1] = rd_s16(dec_data + o + vi * 8 + 2);
                vs[vi][2] = rd_s16(dec_data + o + vi * 8 + 4);
                u16 uv_raw = rd_u16(dec_data + o + vi * 8 + 6);
                uvs[vi][0] = (u8)(uv_raw & 0xFF);
                uvs[vi][1] = (u8)((uv_raw >> 8) & 0xFF);
            }

            u16 tp = rd_u16(dec_data + o + 24);
            u16 cl = rd_u16(dec_data + o + 26);
            u8 cols[3][3];
            for (int ci = 0; ci < 3; ci++) {
                cols[ci][0] = dec_data[o + 28 + ci * 4];
                cols[ci][1] = dec_data[o + 28 + ci * 4 + 1];
                cols[ci][2] = dec_data[o + 28 + ci * 4 + 2];
            }

            u32 bi = (u32)verts.count;
            for (int vi = 0; vi < 3; vi++) {
                f32 lx = (f32)vs[vi][0];
                f32 ly = (f32)vs[vi][1];
                f32 lz = (f32)vs[vi][2];
                if (has_rot) {
                    f32 rx = cr * lx + sr * lz;
                    f32 rz = -sr * lx + cr * lz;
                    lx = rx; lz = rz;
                }
                MeshVert mv = {};
                mv.x = -(lx + tx);
                mv.y = -(ly + ty);
                mv.z = (lz + tz);
                verts.push(mv);
            }
            MeshTri t = {};
            t.alt = is_alt_section ? 1 : 0;
            t.src_off = (u32)off;
            t.idx[0] = bi; t.idx[1] = bi+2; t.idx[2] = bi+1;  /* reversed for X negate */
            t.color = avg_colors(cols, 3);
            set_vcols(t, cols, 0, 2, 1);
            t.tpage = tp; t.clut = cl;
            t.uv[0][0] = uvs[0][0]; t.uv[0][1] = uvs[0][1];
            t.uv[1][0] = uvs[2][0]; t.uv[1][1] = uvs[2][1];
            t.uv[2][0] = uvs[1][0]; t.uv[2][1] = uvs[1][1];
            tris.push(t);
        }

        /* Parse inline quads (52 bytes each) */
        for (int i = 0; i < (int)c2; i++) {
            size_t o = p2 + (size_t)i * 52;
            if (o + 52 > dec_size) break;

            s16 vs[4][3];
            u8  uvs[4][2];
            for (int vi = 0; vi < 4; vi++) {
                vs[vi][0] = rd_s16(dec_data + o + vi * 8);
                vs[vi][1] = rd_s16(dec_data + o + vi * 8 + 2);
                vs[vi][2] = rd_s16(dec_data + o + vi * 8 + 4);
                u16 uv_raw = rd_u16(dec_data + o + vi * 8 + 6);
                uvs[vi][0] = (u8)(uv_raw & 0xFF);
                uvs[vi][1] = (u8)((uv_raw >> 8) & 0xFF);
            }

            u16 tp = rd_u16(dec_data + o + 32);
            u16 cl = rd_u16(dec_data + o + 34);
            u8 cols[4][3];
            for (int ci = 0; ci < 4; ci++) {
                cols[ci][0] = dec_data[o + 36 + ci * 4];
                cols[ci][1] = dec_data[o + 36 + ci * 4 + 1];
                cols[ci][2] = dec_data[o + 36 + ci * 4 + 2];
            }

            u32 bi = (u32)verts.count;
            RGBA8 c = avg_colors(cols, 4);
            for (int vi = 0; vi < 4; vi++) {
                f32 lx = (f32)vs[vi][0];
                f32 ly = (f32)vs[vi][1];
                f32 lz = (f32)vs[vi][2];
                if (has_rot) {
                    f32 rx = cr * lx + sr * lz;
                    f32 rz = -sr * lx + cr * lz;
                    lx = rx; lz = rz;
                }
                MeshVert mv = {};
                mv.x = -(lx + tx);
                mv.y = -(ly + ty);
                mv.z = (lz + tz);
                verts.push(mv);
            }
            MeshTri t1 = {};
            t1.alt = is_alt_section ? 1 : 0;
            t1.src_off = (u32)off;
            t1.idx[0] = bi; t1.idx[1] = bi+2; t1.idx[2] = bi+1;  /* reversed for X negate */
            t1.color = c; t1.tpage = tp; t1.clut = cl;
            set_vcols(t1, cols, 0, 2, 1);
            t1.uv[0][0] = uvs[0][0]; t1.uv[0][1] = uvs[0][1];
            t1.uv[1][0] = uvs[2][0]; t1.uv[1][1] = uvs[2][1];
            t1.uv[2][0] = uvs[1][0]; t1.uv[2][1] = uvs[1][1];
            tris.push(t1);
            MeshTri t2 = {};
            t2.alt = is_alt_section ? 1 : 0;
            t2.src_off = (u32)off;
            t2.idx[0] = bi+1; t2.idx[1] = bi+2; t2.idx[2] = bi+3;  /* reversed */
            t2.color = c; t2.tpage = tp; t2.clut = cl;
            set_vcols(t2, cols, 1, 2, 3);
            t2.uv[0][0] = uvs[1][0]; t2.uv[0][1] = uvs[1][1];
            t2.uv[1][0] = uvs[2][0]; t2.uv[1][1] = uvs[2][1];
            t2.uv[2][0] = uvs[3][0]; t2.uv[2][1] = uvs[3][1];
            tris.push(t2);
        }

        off = quad_end;
    }

    int room_verts = verts.count;
    int room_tris  = tris.count;

    /* ─── Phase 2: DISABLED ───
     * The instance table after inline sections contains EMD character mesh
     * data (skeletal body parts with bone-local positions), NOT room geometry.
     * Including it dumps untransformed body parts at origin as a triangle pile.
     * Character meshes are placed via SCD opcode 0x20 and need proper skeletal
     * animation to render correctly. Skip directly to finalize. */
    goto finalize;

    /* Record 0: shared vertex_ptr + normal_ptr */
    if (off + 8 > dec_size) goto finalize;
    {
        u32 vp_off = rd_u32(dec_data + off) - base;
        u32 np_off = rd_u32(dec_data + off + 4) - base;
        if (vp_off >= dec_size || np_off >= dec_size || np_off <= vp_off)
            goto finalize;

        int n_shared_verts = (int)(np_off - vp_off) / 8;
        if (n_shared_verts < 4 || n_shared_verts > 10000) goto finalize;

        /* Validate: data at vp_off should look like vertices */
        {
            s16 tx = rd_s16(dec_data + vp_off);
            s16 ty = rd_s16(dec_data + vp_off + 2);
            if (tx > 5000 || tx < -5000 || ty > 5000 || ty < -5000)
                goto finalize;
        }

        /* Read shared vertex pool  -  skip padding entries
           (some slots contain ASCII text like "Ryuta..." as separators) */
        Array<MeshVert> shared_verts;
        shared_verts.reserve(n_shared_verts);
        for (int i = 0; i < n_shared_verts; i++) {
            size_t vo = vp_off + (size_t)i * 8;
            if (vo + 6 > dec_size) break;
            MeshVert mv = {};
            s16 sx = rd_s16(dec_data + vo);
            s16 sy = rd_s16(dec_data + vo + 2);
            s16 sz = rd_s16(dec_data + vo + 4);
            mv.x = (f32)sx;
            mv.y = (f32)(-sy);
            mv.z = (f32)sz;
            shared_verts.push(mv);
        }

        /* Skip record 0 (8 bytes) */
        size_t inst_off = off + 8;

        /* Skip record 1 if present (24 bytes: full model aggregate).
           Detect it: same face pointer range as all sub-instances combined.
           It has 4 extra bytes compared to standard 20-byte records. */
        if (inst_off + 24 <= dec_size) {
            u32 r1p1 = rd_u32(dec_data + inst_off);
            u32 r1p2 = rd_u32(dec_data + inst_off + 4);
            u32 r1o1 = r1p1 - base;
            u32 r1o2 = r1p2 - base;
            if (r1o1 < dec_size && r1o2 < dec_size) {
                /* Check if next record (at +24) also starts with valid ptrs */
                if (inst_off + 24 + 8 <= dec_size) {
                    u32 np1 = rd_u32(dec_data + inst_off + 24) - base;
                    u32 np2 = rd_u32(dec_data + inst_off + 28) - base;
                    if (np1 < dec_size && np2 < dec_size) {
                        /* Record 1 is 24 bytes (aggregate)  -  skip it */
                        inst_off += 24;
                    }
                }
            }
        }

        /* Parse instance records (20 bytes each):
           u32 tri_face_ptr, quad_face_ptr
           u16 tri_count, quad_count
           s16 pos_x, pos_y, pos_z, rotation
           The last record may be truncated (12 bytes: ptrs+counts only)
           if it abuts the vertex pool. Use origin position for such cases. */
        int max_instances = 200;
        while (inst_off + 12 <= dec_size && max_instances-- > 0) {
            u32 tri_ptr_off  = rd_u32(dec_data + inst_off) - base;
            u32 quad_ptr_off = rd_u32(dec_data + inst_off + 4) - base;

            /* Stop if pointers aren't valid */
            if (tri_ptr_off >= dec_size || quad_ptr_off >= dec_size)
                break;

            u16 tri_count  = rd_u16(dec_data + inst_off + 8);
            u16 quad_count = rd_u16(dec_data + inst_off + 10);

            /* Position + rotation: available only if full 20-byte record
               fits before the vertex pool. Otherwise use origin (0,0,0). */
            s16 pos_x = 0, pos_y = 0, pos_z = 0, rot_raw = 0;
            if (inst_off + 20 <= vp_off) {
                pos_x   = rd_s16(dec_data + inst_off + 12);
                pos_y   = rd_s16(dec_data + inst_off + 14);
                pos_z   = rd_s16(dec_data + inst_off + 16);
                rot_raw = rd_s16(dec_data + inst_off + 18);
            }

            inst_off += (inst_off + 20 <= vp_off) ? 20 : 12;

            if (tri_count == 0 && quad_count == 0) continue;
            if (tri_count > 5000 || quad_count > 5000) break;

            /* Validate face data ranges */
            if (tri_ptr_off + (size_t)tri_count * 16 > dec_size) continue;
            if (quad_ptr_off + (size_t)quad_count * 20 > dec_size) continue;

            /* Offset into the output vertex array for this instance */
            u32 base_idx = (u32)verts.count;

            /* Precompute Y-axis rotation for this instance */
            f32 rot_angle = (f32)rot_raw * 6.2831853f / 4096.0f;
            f32 cr = cosf(rot_angle);
            f32 sr = sinf(rot_angle);

            /* Copy shared vertices with Y rotation + position offset */
            for (int i = 0; i < (int)shared_verts.count; i++) {
                f32 lx = shared_verts[i].x;
                f32 ly = shared_verts[i].y;
                f32 lz = shared_verts[i].z;
                MeshVert mv = {};
                mv.x = -((cr * lx + sr * lz) + (f32)pos_x);
                mv.y = ly + (f32)(-pos_y);
                mv.z = (-sr * lx + cr * lz) + (f32)pos_z;
                verts.push(mv);
            }

            /* Parse 16-byte indexed triangles:
               u16 idx0, idx1, idx2
               u8  u0, v0
               u8  u1, v1
               u16 CLUT
               u8  u2, v2
               u16 tpage */
            for (int i = 0; i < (int)tri_count; i++) {
                size_t o = tri_ptr_off + (size_t)i * 16;
                if (o + 16 > dec_size) break;

                u16 i0 = rd_u16(dec_data + o);
                u16 i1 = rd_u16(dec_data + o + 2);
                u16 i2 = rd_u16(dec_data + o + 4);
                if (i0 >= (u16)shared_verts.count ||
                    i1 >= (u16)shared_verts.count ||
                    i2 >= (u16)shared_verts.count)
                    continue;

                /* Skip faces referencing padding/separator verts */
                const MeshVert& sv0 = shared_verts[i0];
                const MeshVert& sv1 = shared_verts[i1];
                const MeshVert& sv2 = shared_verts[i2];
                if (sv0.x > MAX_COORD || sv0.x < -MAX_COORD ||
                    sv1.x > MAX_COORD || sv1.x < -MAX_COORD ||
                    sv2.x > MAX_COORD || sv2.x < -MAX_COORD)
                    continue;

                MeshTri t = {};
                t.alt = 0;
                t.idx[0] = base_idx + i0;
                t.idx[1] = base_idx + i2;  /* reversed for X negate */
                t.idx[2] = base_idx + i1;
                t.uv[0][0] = dec_data[o + 6]; t.uv[0][1] = dec_data[o + 7];
                t.uv[1][0] = dec_data[o + 12]; t.uv[1][1] = dec_data[o + 13];
                t.clut     = rd_u16(dec_data + o + 10);
                t.tpage    = rd_u16(dec_data + o + 14);
                t.uv[2][0] = dec_data[o + 8]; t.uv[2][1] = dec_data[o + 9];
                t.color = RGBA8(128, 128, 128, 255);
                tris.push(t);
            }

            /* Parse 20-byte indexed quads:
               u16 idx0, idx1, idx2, idx3
               u8  u0, v0
               u16 CLUT
               u8  u1, v1
               u16 tpage
               u8  u2, v2
               u8  u3, v3 */
            for (int i = 0; i < (int)quad_count; i++) {
                size_t o = quad_ptr_off + (size_t)i * 20;
                if (o + 20 > dec_size) break;

                u16 i0 = rd_u16(dec_data + o);
                u16 i1 = rd_u16(dec_data + o + 2);
                u16 i2 = rd_u16(dec_data + o + 4);
                u16 i3 = rd_u16(dec_data + o + 6);
                if (i0 >= (u16)shared_verts.count ||
                    i1 >= (u16)shared_verts.count ||
                    i2 >= (u16)shared_verts.count ||
                    i3 >= (u16)shared_verts.count)
                    continue;

                /* Skip faces referencing padding/separator verts */
                const MeshVert& sv0 = shared_verts[i0];
                const MeshVert& sv1 = shared_verts[i1];
                if (sv0.x > MAX_COORD || sv0.x < -MAX_COORD ||
                    sv1.x > MAX_COORD || sv1.x < -MAX_COORD)
                    continue;

                u16 tp = rd_u16(dec_data + o + 14);
                u16 cl = rd_u16(dec_data + o + 10);
                RGBA8 col(128, 128, 128, 255);

                /* Quad → 2 triangles, reversed for X negate */
                MeshTri t1 = {};
                t1.alt = 0;
                t1.idx[0] = base_idx + i0;
                t1.idx[1] = base_idx + i2;  /* reversed */
                t1.idx[2] = base_idx + i1;
                t1.color = col; t1.tpage = tp; t1.clut = cl;
                t1.uv[0][0] = dec_data[o + 8];  t1.uv[0][1] = dec_data[o + 9];
                t1.uv[1][0] = dec_data[o + 16]; t1.uv[1][1] = dec_data[o + 17];
                t1.uv[2][0] = dec_data[o + 12]; t1.uv[2][1] = dec_data[o + 13];
                tris.push(t1);

                MeshTri t2 = {};
                t2.alt = 0;
                t2.idx[0] = base_idx + i1;
                t2.idx[1] = base_idx + i2;  /* reversed */
                t2.idx[2] = base_idx + i3;
                t2.color = col; t2.tpage = tp; t2.clut = cl;
                t2.uv[0][0] = dec_data[o + 12]; t2.uv[0][1] = dec_data[o + 13];
                t2.uv[1][0] = dec_data[o + 16]; t2.uv[1][1] = dec_data[o + 17];
                t2.uv[2][0] = dec_data[o + 18]; t2.uv[2][1] = dec_data[o + 19];
                tris.push(t2);
            }
        }
    }

finalize:
    if (tris.count == 0) return false;

    /* Copy to output mesh */
    mesh.alloc(verts.count, tris.count);
    for (int i = 0; i < (int)verts.count; i++)
        mesh.verts[i] = verts[i];
    for (int i = 0; i < (int)tris.count; i++)
        mesh.tris[i] = tris[i];

    (void)room_verts; (void)room_tris;
    return true;
}


bool parse_rdt_scene(const u8* data, size_t size, u32 base_addr, Mesh& mesh)
{
    Buffer dec;
    if (!lzss_decompress(data, size, dec)) return false;
    return parse_rdt_scene_dec(dec.data, dec.size, base_addr, mesh);
}

/*═══════════════════════════════════════════════════════════════════
 *  OBJ Export
 *═══════════════════════════════════════════════════════════════════*/
bool export_mesh_obj(const char* path, const Mesh& mesh)
{
    FILE* f = fopen(path, "w");
    if (!f) return false;

    /* Game units are about a millimetre; write metres, as GLB export does,
       so a room imports at its real size instead of kilometres across. */
    const f32 unit = 0.001f;

    fprintf(f, "# RaptorScope - Dino Crisis Mesh Export\n");
    fprintf(f, "# Vertices: %d  Faces: %d  Units: metres\n\n", mesh.vert_count, mesh.tri_count);

    /* Vertices with positions (transforms already baked into mesh.verts) */
    for (int i = 0; i < mesh.vert_count; i++)
        fprintf(f, "v %f %f %f\n", mesh.verts[i].x * unit, mesh.verts[i].y * unit,
                mesh.verts[i].z * unit);

    fprintf(f, "\n");

    /* Normals */
    bool has_normals = false;
    for (int i = 0; i < mesh.vert_count; i++) {
        const MeshVert& v = mesh.verts[i];
        if (v.nx != 0 || v.ny != 0 || v.nz != 0) { has_normals = true; break; }
    }
    if (has_normals) {
        for (int i = 0; i < mesh.vert_count; i++)
            fprintf(f, "vn %f %f %f\n", mesh.verts[i].nx, mesh.verts[i].ny, mesh.verts[i].nz);
        fprintf(f, "\n");
    }

    /* Per-face vertex colors (xOBJ/MeshLab extension: "vc R G B" per face) */
    /* We emit per-face color as a comment, and also write a companion .mtl
       with per-face materials if desired. For Blender compatibility,
       embed colors via the unofficial "# vc r g b a" comment per face. */
    fprintf(f, "# Per-face vertex colors (R G B 0-255):\n");
    for (int i = 0; i < mesh.tri_count; i++) {
        const MeshTri& t = mesh.tris[i];
        if (has_normals)
            fprintf(f, "f %u//%u %u//%u %u//%u",
                t.idx[0]+1, t.idx[0]+1, t.idx[1]+1, t.idx[1]+1, t.idx[2]+1, t.idx[2]+1);
        else
            fprintf(f, "f %u %u %u", t.idx[0]+1, t.idx[1]+1, t.idx[2]+1);
        fprintf(f, " # vc %d %d %d\n", t.color.r, t.color.g, t.color.b);
    }

    fclose(f);
    return true;
}

/*═══════════════════════════════════════════════════════════════════
 *  SMD Export — Valve StudioModel Data
 *  Supports: bones (hierarchy), bone-weighted vertices, triangles,
 *  normals, UVs, materials. Skinned meshes export in bind pose
 *  with bone transforms baked from the EMD skeleton.
 *═══════════════════════════════════════════════════════════════════*/
bool export_emd_smd(const char* path, const EmdModel& emd)
{
    if (!emd.valid) return false;
    FILE* f = fopen(path, "w");
    if (!f) return false;

    const Mesh& mesh = emd.mesh;
    const EmdSkeleton& skel = emd.skeleton;

    fprintf(f, "version 1\n");

    /* ── Bone hierarchy ── */
    fprintf(f, "nodes\n");
    for (int i = 0; i < skel.bone_count; i++) {
        const char* name = get_bone_name(skel.bone_count, i);
        char fallback[32];
        if (!name) { _snprintf(fallback, 31, "bone_%02d", i); name = fallback; }
        int parent = (i == 0) ? -1 : (int)skel.bones[i].parent;
        fprintf(f, "  %d \"%s\" %d\n", i, name, parent);
    }
    fprintf(f, "end\n");

    /* ── Bind pose (frame 0) ── */
    fprintf(f, "skeleton\n");
    fprintf(f, "time 0\n");
    for (int i = 0; i < skel.bone_count; i++) {
        /* Local offset from parent */
        float tx = (float)skel.bones[i].bx;
        float ty = (float)skel.bones[i].by;
        float tz = (float)skel.bones[i].bz;
        /* No rotation in bind pose (identity) */
        fprintf(f, "  %d  %f %f %f  0.000000 0.000000 0.000000\n", i, tx, ty, tz);
    }
    fprintf(f, "end\n");

    /* ── Triangles ── */
    fprintf(f, "triangles\n");
    for (int i = 0; i < mesh.tri_count; i++) {
        const MeshTri& t = mesh.tris[i];

        /* Material name from tpage */
        fprintf(f, "tpage_%02d\n", t.tpage & 0x1F);

        for (int v = 0; v < 3; v++) {
            int vi = t.idx[v];
            if (vi >= mesh.vert_count) continue;
            const MeshVert& mv = mesh.verts[vi];

            /* Determine bone assignment */
            int bone = 0;
            if (emd.vert_src && vi < emd.vert_src_count)
                bone = emd.vert_src[vi].dest_bone;
            if (bone < 0 || bone >= skel.bone_count) bone = 0;

            /* Position (world space — already transformed by skeleton) */
            float px = mv.x, py = mv.y, pz = mv.z;
            float nx = mv.nx, ny = mv.ny, nz = mv.nz;
            float nlen = sqrtf(nx*nx + ny*ny + nz*nz);
            if (nlen < 0.001f) { nx = 0; ny = 1; nz = 0; }

            /* UV */
            float u = t.uv[v][0] / 255.0f;
            float fv = t.uv[v][1] / 255.0f;

            /* SMD format: parent_bone  px py pz  nx ny nz  u v  [links] */
            fprintf(f, "  %d  %f %f %f  %f %f %f  %f %f  1  %d 1.000000\n",
                bone, px, py, pz, nx, ny, nz, u, fv, bone);
        }
    }
    fprintf(f, "end\n");

    fclose(f);
    return true;
}

/* Export static mesh as SMD (no bones, single root bone) */
bool export_mesh_smd(const char* path, const Mesh& mesh)
{
    FILE* f = fopen(path, "w");
    if (!f) return false;

    fprintf(f, "version 1\n");
    fprintf(f, "nodes\n");
    fprintf(f, "  0 \"root\" -1\n");
    fprintf(f, "end\n");
    fprintf(f, "skeleton\n");
    fprintf(f, "time 0\n");
    fprintf(f, "  0  0.000000 0.000000 0.000000  0.000000 0.000000 0.000000\n");
    fprintf(f, "end\n");
    fprintf(f, "triangles\n");
    for (int i = 0; i < mesh.tri_count; i++) {
        const MeshTri& t = mesh.tris[i];
        fprintf(f, "tpage_%02d\n", t.tpage & 0x1F);
        for (int v = 0; v < 3; v++) {
            int vi = t.idx[v];
            if (vi >= mesh.vert_count) continue;
            const MeshVert& mv = mesh.verts[vi];
            float nx = mv.nx, ny = mv.ny, nz = mv.nz;
            float nlen = sqrtf(nx*nx + ny*ny + nz*nz);
            if (nlen < 0.001f) { nx = 0; ny = 1; nz = 0; }
            float u = t.uv[v][0] / 255.0f;
            float fv = t.uv[v][1] / 255.0f;
            fprintf(f, "  0  %f %f %f  %f %f %f  %f %f\n",
                mv.x, mv.y, mv.z, nx, ny, nz, u, fv);
        }
    }
    fprintf(f, "end\n");
    fclose(f);
    return true;
}

/*═══════════════════════════════════════════════════════════════════
 *  Bone Name Lookup (auto-detect skeleton type by bone count)
 *═══════════════════════════════════════════════════════════════════*/
static const char* s_human_15[] = {
    "root","shoulder_L","elbow_L","hand_L","shoulder_R","elbow_R","hand_R",
    "neck","torso","thigh_L","knee_L","foot_L","thigh_R","knee_R","foot_R"
};
static const char* s_compy_7[] = {
    "root","neck","head","thigh_R","thigh_L","tail_0","tail_1"
};
static const char* s_pteranodon_18[] = {
    "root","neck","head","jaw",
    "shoulder_L","elbow_L","hand_L","wing_L",
    "shoulder_R","elbow_R","hand_R","wing_R",
    "thigh_R","knee_R","foot_R","thigh_L","knee_L","foot_L"
};
static const char* s_trex_20[] = {
    "root","torso","neck","head","jaw","shoulder_R","shoulder_L",
    "thigh_R","knee_R","foot_R","toes_R",
    "thigh_L","knee_L","foot_L","toes_L",
    "tail_0","tail_1","tail_2","tail_3","tail_4"
};
static const char* s_raptor_21[] = {
    "root","torso","neck","head","jaw",
    "shoulder_R","hand_R","shoulder_L","hand_L",
    "thigh_R","knee_R","foot_R","toes_R",
    "tail_0","tail_1","tail_2","tail_3",
    "thigh_L","knee_L","foot_L","toes_L"
};
static const char* s_therizino_22[] = {
    "root","torso","neck","head","jaw",
    "shoulder_R","elbow_R","hand_R","shoulder_L","elbow_L","hand_L",
    "thigh_R","knee_R","foot_R","toes_R",
    "thigh_L","knee_L","foot_L","toes_L",
    "tail_0","tail_1","tail_2"
};

const char* get_bone_name(int bone_count, int bone_idx)
{
    const char** tbl = 0;
    int n = 0;
    switch (bone_count) {
        case  7: tbl = s_compy_7;       n =  7; break;
        case 15: tbl = s_human_15;      n = 15; break;
        case 18: tbl = s_pteranodon_18; n = 18; break;
        case 20: tbl = s_trex_20;       n = 20; break;
        case 21: tbl = s_raptor_21;     n = 21; break;
        case 22: tbl = s_therizino_22;  n = 22; break;
    }
    if (tbl && bone_idx >= 0 && bone_idx < n) return tbl[bone_idx];
    return 0;
}

/*═══════════════════════════════════════════════════════════════════
 *  parse_rdt_overlay  –  Extract non-mesh spatial data from RDT blob
 *
 *  Reads the 7 header pointers and extracts:
 *    - Floor zones (ptr[6])     → trigger/transition quads
 *    - Collision rects (ptr[1]) → walkable area rectangles
 *    - Camera data (ptr[2])     → camera eye/target positions
 *    - Slot table (ptr[4])      → 10 runtime slots, no static data
 *    - SCD opcodes (ptr[5])     → model transforms, object spawns,
 *                                  character placements
 *═══════════════════════════════════════════════════════════════════*/
/*═══════════════════════════════════════════════════════════════════
 *  parse_rdt_overlay  –  SCD Script Interpreter + Header Tables
 *
 *  Walks the SCD bytecode using the game's actual opcode sizes
 *  (extracted from DINO.exe dispatch table at VA 0x6576A0).
 *
 *  Extracts:
 *    - Collision rects (ptr[1])     → walkable floor areas
 *    - Collision/trigger zones (0x28) → interaction quads
 *    - Model transforms (0x23)      → section positioning
 *    - Object spawns (0x42)         → dynamic item placement
 *    - Character placements (0x20)  → EMD enemy/NPC positions
 *    - Camera setups (0x4C)         → eye/target positions
 *    - Floor zones (ptr[6])         → trigger/transition quads
 *═══════════════════════════════════════════════════════════════════*/

/* Section placements: every 0x23 opcode (model slot = section at a
   position) and every item pickup zone (0x28 type 4) that shows a model.
   The linear script walk loses sync on some scripts and misses
   placements, so scan the whole script area instead.  Opcodes start on
   4-byte boundaries from the script base, and a candidate only counts
   when its pointer lands on a valid section header, so false hits are
   not a concern.  The same section at the same place is kept once.

   Scripts then often adjust a placed model: 0x22 03 <slot> selects model
   slot <slot> as the work target, then 0x2A sets one of its fields (field
   3/4/5 = x/y/z, 6/7/8 = rotation; sub_47416C), 0x36 sets x/y/z and 0x37
   the rotation.  Item models rely on this, e.g. ST302 lifts its DDK onto
   the desk with y = -950.  The writes up to the next work target are
   applied to the slot's latest placement; positions beyond +/-30000 are
   scripts moving a model out of view and are skipped. */
static void scan_section_placements(const u8* dec, size_t dec_size, u32 base,
                                    size_t scd_base, size_t meta_min,
                                    RdtSceneOverlay& ov)
{
    int slot_xf[256];
    for (int i = 0; i < 256; i++) slot_xf[i] = -1;

    for (size_t pc = scd_base; pc + 32 <= dec_size && ov.n_xforms < 64; pc += 4) {
        u8 op = dec[pc];
        u32 sec_off;
        RdtSectionXform x;
        memset(&x, 0, sizeof(x));
        if (op == 0x22 && dec[pc + 1] == 3 && dec[pc + 3] == 0) {
            int xi = slot_xf[dec[pc + 2]];
            size_t q = pc + 4;
            for (int n = 0; xi >= 0 && n < 32 && q + 8 <= dec_size; n++) {
                u8 qop = dec[q];
                if (qop == 0x22) break;              /* new work target */
                RdtSectionXform& t = ov.xforms[xi];
                if (qop == 0x2A) {
                    s16 v = rd_s16(dec + q + 4);
                    switch (dec[q + 2]) {
                    case 3: if (v > -30000 && v < 30000) t.px = v; break;
                    case 4: if (v > -30000 && v < 30000) t.py = v; break;
                    case 5: if (v > -30000 && v < 30000) t.pz = v; break;
                    case 6: t.rx = v; break;
                    case 7: t.ry = v; break;
                    case 8: t.rz = v; break;
                    }
                } else if (qop == 0x36 || qop == 0x37) {
                    /* 0x36 sets x/y/z (fields 3-5), 0x37 the rotation (6-8) */
                    s16 a = rd_s16(dec + q + 2), b = rd_s16(dec + q + 4), c = rd_s16(dec + q + 6);
                    if (qop == 0x37) { t.rx = a; t.ry = b; t.rz = c; }
                    else if (a > -30000 && a < 30000 && b > -30000 && b < 30000 &&
                             c > -30000 && c < 30000) { t.px = a; t.py = b; t.pz = c; }
                }
                int adv = scd_inst_size(dec, dec_size, q);
                if (adv == 0) break;
                q += (size_t)adv;
            }
            continue;
        }
        if (op == 0x23) {
            sec_off = rd_u32(dec + pc + 8) - base;
            x.slot        = dec[pc + 1];
            x.flags       = dec[pc + 2];
            x.render_mode = dec[pc + 3];
            x.ot_depth    = rd_u16(dec + pc + 4);
            x.px = rd_s16(dec + pc + 12);
            x.py = rd_s16(dec + pc + 14);
            x.pz = rd_s16(dec + pc + 16);
            x.rx = rd_s16(dec + pc + 18);
            x.ry = rd_s16(dec + pc + 20);
            x.rz = rd_s16(dec + pc + 22);
        } else if (op == 0x28 && dec[pc + 2] == 4 && pc + 44 <= dec_size &&
                   dec[pc + 34] != 0xFF) {
            /* Item pickup zone: model slot at +34, section at +36.  The
               game (sub_426DFC -> sub_448E3B) puts the model on the floor
               (y = 0) at the midpoint of zone corners 0 and 2, and spins
               it about Y while the item has not been picked up. */
            sec_off = rd_u32(dec + pc + 36) - base;
            s16 a0 = rd_s16(dec + pc + 4);
            s16 a1 = rd_s16(dec + pc + 6);
            s16 a4 = rd_s16(dec + pc + 12);
            s16 a5 = rd_s16(dec + pc + 14);
            x.slot = dec[pc + 34];
            x.px = a4 + (s16)((a0 - a4) / 2);
            x.pz = a5 + (s16)((a1 - a5) / 2);
        } else {
            continue;
        }

        if (sec_off < 0x1C || sec_off + 12 > meta_min) continue;
        u32 sp1 = rd_u32(dec + sec_off) - base;
        u32 sp2 = rd_u32(dec + sec_off + 4) - base;
        u16 sc1 = rd_u16(dec + sec_off + 8);
        u16 sc2 = rd_u16(dec + sec_off + 10);
        if (sp1 >= dec_size || sp2 >= dec_size || sc1 > 5000 || sc2 > 5000) continue;
        if ((size_t)sp1 + (size_t)sc1 * 40 > dec_size ||
            (size_t)sp2 + (size_t)sc2 * 52 > dec_size) continue;
        x.section_off = sec_off;

        int found = -1;
        for (int i = 0; i < ov.n_xforms && found < 0; i++) {
            const RdtSectionXform& o = ov.xforms[i];
            if (o.section_off == x.section_off &&
                o.px == x.px && o.py == x.py && o.pz == x.pz &&
                o.rx == x.rx && o.ry == x.ry && o.rz == x.rz) found = i;
        }
        if (found < 0) { found = ov.n_xforms; ov.xforms[ov.n_xforms++] = x; }
        slot_xf[x.slot] = found;
    }
}

/* Trigger zones: every 0x28 opcode in the script area.  The script walk
   stops at the first 0x01, but 0x01 is a 4-byte opcode that scripts
   continue past, so it missed most zones in rooms like ST103 (its door
   triggers among them).  As with section placements, scan the whole area
   at 4-byte steps instead.  A real zone repeats its type at +20 and has 1
   at +23; every 0x28 in all rooms that is not a zone (bytes of embedded
   data tables) fails that test.  Scripts set the same zone from several
   branches, so identical type + corners are kept once.  Returns the
   number of zones added. */
static int scan_trigger_zones(const u8* dec, size_t scd_base, size_t scd_end,
                              RdtSceneOverlay& ov)
{
    int added = 0;
    for (size_t pc = scd_base; pc + 24 <= scd_end && ov.n_zones < 64; pc += 4) {
        u8 typ = dec[pc + 2];
        if (dec[pc] != 0x28 || typ >= 12 || dec[pc + 20] != typ || dec[pc + 23] != 1)
            continue;
        RdtFloorZone z;
        memset(&z, 0, sizeof(z));
        for (int c = 0; c < 4; c++) {
            z.x[c] = rd_s16(dec + pc + 4 + c * 4);
            z.z[c] = rd_s16(dec + pc + 4 + c * 4 + 2);
        }
        z.collision_group = dec[pc + 1];   /* zone slot */
        z.target_index    = typ;
        z.kind            = RDT_ZONE_TRIGGER;
        bool dup = false;
        for (int i = 0; i < ov.n_zones && !dup; i++) {
            const RdtFloorZone& o = ov.zones[i];
            dup = o.kind == RDT_ZONE_TRIGGER && o.target_index == typ &&
                  memcmp(o.x, z.x, sizeof(z.x)) == 0 && memcmp(o.z, z.z, sizeof(z.z)) == 0;
        }
        if (dup) continue;
        ov.zones[ov.n_zones++] = z;
        added++;
    }
    return added;
}

/* Enemy spawns (0x5B, 44 bytes).  The handler (0x42A80D in the GOG exe,
   0x42A96D in the other build, C13) puts a character
   into the same 0x260-byte character table as 0x20: byte[1] = character
   slot, byte[2] = type (+0x27), byte[3] (+0x2F), position at +4/+6/+8,
   rotation at +10, model and animation pointers at +12/+16; the game
   writes its spawn progress back into the opcode at +0x22.  Found by
   scanning the script area rather than by the walk, which used to skip
   the branches that hold them (ST309, ST504 and five more).  Both pointers lie in the room
   data and differ; mesh data that happens to match has them equal or a
   slot past the table.

   A cutscene can spawn its enemy off stage and bring it on later: ST100's
   raptor (thread 6) is spawned at x = z = 0x7000, far outside the room,
   and appears when the cutscene moves it to (2024, 0, 3008).  Every other
   spawn in the game lies within +/-15104, so one beyond +/-20000 is shown
   where its thread first moves it on stage (enemy_spawn_entrance), or
   left out if it never does. */
static bool spawn_on_stage(s16 x, s16 z)
{
    return x > -20000 && x < 20000 && z > -20000 && z < 20000;
}

/* The first move of a parked 0x5B spawn's enemy onto the stage by the
   rest of its thread; scd/scd_size are the script area and pc is the
   spawn's offset in it.  0x22 02 <slot> selects entry <slot> of the
   character table, the one the spawn filled, as the work target
   (sub_47416C, work type 2: +0x288 + slot * 0x260).  0x36 then sets x/y/z
   (fields 3-5, entry +0x20/+0x22/+0x24, where 0x5B puts its position),
   0x2A one field, and 0x37 the rotation (fields 6-8; field 7 is the Y
   angle 0x5B sets).  The writes of the first work-target block that
   leaves the enemy on stage are applied to es. */
static bool enemy_spawn_entrance(const u8* scd, size_t scd_size, size_t pc, RdtItemSpawn& es)
{
    u32 thr[64];
    int n_thr = 0;
    u32 first = rd_u32(scd);
    if (first >= 4 && !(first & 3) && first <= scd_size)
        for (; n_thr < (int)(first / 4) && n_thr < 64; n_thr++) thr[n_thr] = rd_u32(scd + n_thr * 4);
    size_t end = scd_thread_end(scd, scd_size, thr, n_thr, pc);

    bool sel = false, moved = false;
    s16 x = 0, y = 0, z = 0, ry = 0;
    for (size_t q = pc + 44; ; ) {
        bool last = q + 8 > end;
        if (last || scd[q] == 0x22) {
            if (sel && moved && spawn_on_stage(x, z)) {
                es.px = x; es.py = y; es.pz = z; es.rot_y = ry;
                return true;
            }
            if (last) return false;
            sel = scd[q + 1] == 2 && scd[q + 2] == es.slot;
            moved = false;
            x = es.px; y = es.py; z = es.pz; ry = es.rot_y;
        } else if (sel && scd[q] == 0x36) {
            x = rd_s16(scd + q + 2); y = rd_s16(scd + q + 4); z = rd_s16(scd + q + 6);
            moved = true;
        } else if (sel && scd[q] == 0x37) {
            ry = rd_s16(scd + q + 4);
        } else if (sel && scd[q] == 0x2A) {
            s16 v = rd_s16(scd + q + 4);
            switch (scd[q + 2]) {
            case 3: x = v; moved = true; break;
            case 4: y = v; moved = true; break;
            case 5: z = v; moved = true; break;
            case 7: ry = v; break;
            }
        }
        int adv = scd_inst_size(scd, scd_size, q);
        q = adv ? q + (size_t)adv : end;
    }
}

static int scan_enemy_spawns(const u8* dec, size_t scd_base, size_t scd_end, u32 base,
                             size_t dec_size, RdtSceneOverlay& ov)
{
    int added = 0;
    for (size_t pc = scd_base; pc + 44 <= scd_end && ov.n_items < 32; pc += 4) {
        if (dec[pc] != 0x5B || dec[pc + 1] >= 32) continue;
        u32 m = rd_u32(dec + pc + 12), a = rd_u32(dec + pc + 16);
        if (m == a || m < base || a < base || m - base >= dec_size || a - base >= dec_size) continue;
        RdtItemSpawn is;
        is.slot     = dec[pc + 1];
        is.type     = dec[pc + 2];
        is.anim_set = dec[pc + 3];
        is.px       = rd_s16(dec + pc + 4);
        is.py       = rd_s16(dec + pc + 6);
        is.pz       = rd_s16(dec + pc + 8);
        is.rot_y    = rd_s16(dec + pc + 10);
        is.mesh_ptr = m;
        is.anim_ptr = a;
        is.src_room = dec[pc + 0x14];
        is.plain    = dec[pc + 0x19] != 0;
        if (!spawn_on_stage(is.px, is.pz) &&
            !enemy_spawn_entrance(dec + scd_base, scd_end - scd_base, pc - scd_base, is))
            continue;
        bool dup = false;
        for (int i = 0; i < ov.n_items && !dup; i++) {
            const RdtItemSpawn& o = ov.items[i];
            dup = o.slot == is.slot && o.type == is.type && o.px == is.px && o.py == is.py &&
                  o.pz == is.pz && o.rot_y == is.rot_y;
        }
        if (dup) continue;
        ov.items[ov.n_items++] = is;
        added++;
    }
    return added;
}

/* Camera setups: the model slots each camera shows and hides.  When the
   game switches to camera N (cam_id of a ptr[3] descriptor) it runs script
   thread N, which uses 0x48 03 <slot> <1|0> to set or clear the slot's
   draw bit (sub_42883F; the room draw loop skips slots without it).  A
   thread runs to the next thread's start or its 0x04. */
static void scan_camera_views(const u8* dec, size_t scd_base, size_t scd_end,
                              RdtSceneOverlay& ov)
{
    if (scd_base + 4 > scd_end) return;
    u32 first = rd_u32(dec + scd_base);
    if (first < 4 || (first & 3) || scd_base + first > scd_end) return;
    int n_threads = (int)(first / 4);

    for (int c = 0; c < ov.n_cam_threads && ov.n_cam_views < 32; c++) {
        int id = ov.cam_threads[c].cam_id;
        if (id == 0 || id >= n_threads) continue;
        bool seen = false;
        for (int v = 0; v < ov.n_cam_views && !seen; v++) seen = ov.cam_views[v].cam_id == id;
        if (seen) continue;

        size_t pc = scd_base + rd_u32(dec + scd_base + id * 4);
        size_t end = scd_end;
        for (int t = 0; t < n_threads; t++) {
            size_t s = scd_base + rd_u32(dec + scd_base + t * 4);
            if (s > pc && s < end) end = s;
        }
        RdtSceneOverlay::CamView cv;
        memset(&cv, 0, sizeof(cv));
        cv.cam_id = (u8)id;
        while (pc + 4 <= end && dec[pc] != 0x04) {
            if (dec[pc] == 0x48 && dec[pc + 1] == 3 && dec[pc + 2] < 64) {
                u64 bit = (u64)1 << dec[pc + 2];
                if (dec[pc + 3] == 1)      { cv.show_mask |= bit; cv.hide_mask &= ~bit; }
                else if (dec[pc + 3] == 0) { cv.hide_mask |= bit; cv.show_mask &= ~bit; }
            }
            int n = scd_inst_size(dec, end, pc);
            if (n <= 0) break;
            pc += (size_t)n;
        }
        if (cv.hide_mask) ov.cam_views[ov.n_cam_views++] = cv;
    }

    /* In camera order, for cycling through them */
    for (int i = 1; i < ov.n_cam_views; i++)
        for (int j = i; j > 0 && ov.cam_views[j].cam_id < ov.cam_views[j - 1].cam_id; j--) {
            RdtSceneOverlay::CamView t = ov.cam_views[j];
            ov.cam_views[j] = ov.cam_views[j - 1];
            ov.cam_views[j - 1] = t;
        }
}

/* Adds a camera shot unless the same eye and target are already stored.
   Scripts repeat shots, in several threads and on both sides of a
   branch, so without this one shot can fill the table. */
static bool add_camera(RdtSceneOverlay& ov, const RdtCameraEntry& cam)
{
    if (ov.n_cameras >= 32) return false;
    for (int i = 0; i < ov.n_cameras; i++) {
        const RdtCameraEntry& o = ov.cameras[i];
        if (o.eye_x == cam.eye_x && o.eye_y == cam.eye_y && o.eye_z == cam.eye_z &&
            o.tgt_x == cam.tgt_x && o.tgt_y == cam.tgt_y && o.tgt_z == cam.tgt_z)
            return false;
    }
    ov.cameras[ov.n_cameras++] = cam;
    return true;
}

bool parse_rdt_overlay(const u8* dec, size_t dec_size, u32 base_addr,
                       RdtSceneOverlay& ov)
{
    if (dec_size < 32) return false;
    u32 base = base_addr;

    /* Validate RDT header: 7 PSX pointers */
    u32 offs[7];
    for (int i = 0; i < 7; i++) {
        u32 p = rd_u32(dec + i * 4);
        if (p < base) return false;
        offs[i] = p - base;
        if (offs[i] >= dec_size) return false;
    }

    /* Determine mesh region end */
    size_t meta_min = dec_size;
    for (int i = 0; i < 7; i++)
        if (offs[i] > 0x40 && offs[i] < meta_min) meta_min = offs[i];

    int found = 0;

    /* ─── Static Room Lights (ptr[0]+36) ───
     * ptr[0] header: u16 n_bank1, u16 n_bank2, u16 pad, u16 flags,
     *   u8 ambient_r, u8 ambient_g, u8 ambient_b, ...
     * At +36: (n_bank1 + n_bank2) × 20-byte light entries.
     * Each entry (from sub_5F0CE0):
     *   byte[0]=R, [1]=G, [2]=B, [3]=active
     *   s16[+4]=X, s16[+6]=Y, s16[+8]=Z, s16[+10]=radius */
    {
        size_t p0 = offs[0];
        if (p0 + 36 <= dec_size) {
            /* Read ambient from header */
            ov.ambient_r = dec[p0 + 8];
            ov.ambient_g = dec[p0 + 9];
            ov.ambient_b = dec[p0 + 10];
            int n1 = (int)rd_u16(dec + p0);
            int n2 = (int)rd_u16(dec + p0 + 2);
            int total = n1 + n2;
            if (total > 0 && total <= 8) {
                for (int li = 0; li < total && ov.n_lights < 8; li++) {
                    size_t lo = p0 + 36 + (size_t)li * 20;
                    if (lo + 20 > dec_size) break;
                    u8 active = dec[lo + 3];
                    if (!active) continue;
                    RdtSceneLight& sl = ov.lights[ov.n_lights++];
                    sl.slot = (u8)li;
                    sl.r = dec[lo];
                    sl.g = dec[lo + 1];
                    sl.b = dec[lo + 2];
                    sl.px = rd_s16(dec + lo + 4);
                    sl.py = rd_s16(dec + lo + 6);
                    sl.pz = rd_s16(dec + lo + 8);
                    sl.radius = rd_s16(dec + lo + 10);
                    sl.has_pos = 1;
                    sl.has_color = 1;
                }
                if (ov.n_lights > 0) found++;
            }
        }
    }

    /* ─── Floor Zones (ptr[6]) ───
     * Areas where footsteps play another sound.  The room loader copies
     * ptr[6] to work +0x26C (DINO.exe 0x42C975): a count byte and three
     * bytes of padding, then 20-byte records of four XZ corners, a level at
     * +16 and a step at +17.  Count and records end exactly at ptr[5]
     * (ptr[6] + 4 + 20 * count) in 147 of the 148 rooms, ST60E being the
     * exception; ST206 has 0x24 in the padding, so it is not a u32.  The
     * footstep player (0x40B8A9, called from the walk code at 0x45ACA0 with
     * sound 0xB0 or 0xB1) adds 2 * step to the sound when the walker's +0x48
     * equals the level and its position is inside the quad.  Opcode 0x64
     * (0x42BE92) sets record N's step, as ST100 does to turn its two zones on.
     * ST300's one zone switches to sounds 0xB2/0xB3, which the PC build's
     * sound list for that room (DINO.exe 0x62A4C8) does not have. */
    {
        size_t zo = offs[6];
        if (zo + 4 <= dec_size) {
            int count = (int)dec[zo];
            if (count > 0 && count <= 64) {
                size_t rec = zo + 4;
                for (int i = 0; i < count && ov.n_zones < 64; i++, rec += 20) {
                    if (rec + 20 > dec_size) break;
                    RdtFloorZone& z = ov.zones[ov.n_zones++];
                    for (int c = 0; c < 4; c++) {
                        z.x[c] = rd_s16(dec + rec + c * 4);
                        z.z[c] = rd_s16(dec + rec + c * 4 + 2);
                    }
                    z.y               = 0;
                    z.height          = 0;
                    z.collision_group = dec[rec + 16];
                    z.target_index    = dec[rec + 17];
                    z.flags           = dec[rec + 18];
                    z.kind            = RDT_ZONE_FLOOR;
                }
                found++;
            }
        }
    }

    /* ─── Collision Rects (ptr[1]) ───
     * Fixed 16-entry index table (16 × 8B = 128 bytes).
     * Each entry: u8 count, u8 flags, u16 rect_offset (relative to entry), u32 extra.
     * Rect data: 48 bytes each, x(s16) z(s16) w(u16) h(u16) + 40B metadata. */
    {
        size_t co = offs[1];
        size_t co_end = offs[2];  /* section ends at ptr[2] */
        for (int gi = 0; gi < 16; gi++) {
            size_t go = co + (size_t)gi * 8;
            if (go + 8 > co_end) break;
            int cnt = (int)dec[go];
            if (cnt == 0) continue;
            if (cnt > 32) continue;   /* skip bogus, don't break */
            u16 rect_off = rd_u16(dec + go + 2);
            size_t rbase = go + rect_off;
            for (int ri = 0; ri < cnt && ov.n_collisions < 256; ri++) {
                size_t ro = rbase + (size_t)ri * 48;
                if (ro + 48 > dec_size) break;
                if (ro + 8 > co_end) break; /* don't read past section */
                s16 rx = rd_s16(dec + ro);
                s16 rz = rd_s16(dec + ro + 2);
                u16 rw = rd_u16(dec + ro + 4);
                u16 rh = rd_u16(dec + ro + 6);
                if (rw == 0 || rh == 0) continue;  /* skip zero-dimension */
                RdtCollisionRect& cr = ov.collisions[ov.n_collisions++];
                cr.x = rx;
                cr.z = rz;
                cr.w = rw;
                cr.h = rh;
                cr.group   = (u8)gi;
                cr.flags   = dec[ro + 8];
            }
        }
        if (ov.n_collisions > 0) found++;
    }

    /* ptr[4] is a runtime pointer table (10 × u32 slots, 40 bytes: ptr[6] - ptr[4]
       is 40 in 147 of the 148 rooms, ST60E being the exception) filled by SCD
       opcode 0x28.  The static data here is just uninitialized slot storage — not
       zone geometry.
       Door zones come from 0x28 type 4 in the SCD walk below. */

    /* ─── Camera Thread Zones (ptr[3]) ───
     * u32 count, then count × 68-byte thread descriptors (word_645F58 = 68 for all types).
     * Each descriptor: byte[0]=type, byte[1]=flags, byte[2]=cam_id, byte[3]=floor_grp,
     * bytes[4..19] = 4 × (s16 x, s16 z) = camera activation quad.
     * Remaining 48 bytes = camera params (eye/target, interpolation, etc). */
    {
        size_t t3 = offs[3];
        size_t t3_end = offs[4];
        if (t3 + 4 <= dec_size) {
            int thr_count = (int)rd_u32(dec + t3);
            if (thr_count > 0 && thr_count <= 32) {
                for (int ti = 0; ti < thr_count && ov.n_cam_threads < 32; ti++) {
                    size_t td = t3 + 4 + (size_t)ti * 68;
                    if (td + 68 > dec_size) break;
                    if (td + 68 > t3_end) break;
                    RdtSceneOverlay::CamThread& ct = ov.cam_threads[ov.n_cam_threads++];
                    ct.type = dec[td];
                    ct.cam_id = dec[td + 2];
                    memcpy(ct.desc, dec + td, 68);
                    for (int c = 0; c < 4; c++) {
                        ct.x[c] = rd_s16(dec + td + 4 + c * 4);
                        ct.z[c] = rd_s16(dec + td + 4 + c * 4 + 2);
                    }
                }
                if (ov.n_cam_threads > 0) found++;
            }
        }
    }

    /* ─── Camera Cut Zones (ptr[2]) ───
     * 16-byte header, then up to 17 camera group descriptors (16 bytes each, indices 0-16).
     * Each group: byte[0] = zone_count, u32 at +4 = zone_data_offset (rel to ptr[2]+272).
     * Zone data: 20-byte entries: type(u8), flags(u8), pad(u16), x(s16), z(s16), w(s16), h(s16), ...
     */
    {
        size_t ct = offs[2];
        if (ct + 16 + 17 * 16 <= dec_size) {
            for (int g = 0; g < 17; g++) {
                size_t goff = ct + 16 + (size_t)g * 16;
                if (goff + 16 > dec_size) break;
                int zone_count = dec[goff];
                if (zone_count == 0 || zone_count > 32) continue;
                u32 zone_reloff = rd_u32(dec + goff + 4);
                size_t zone_abs = ct + 272 + zone_reloff;
                for (int z = 0; z < zone_count && ov.n_camcuts < 128; z++) {
                    size_t zo = zone_abs + (size_t)z * 20;
                    if (zo + 20 > dec_size) break;
                    /* Validate: w and h at +8,+10 must be nonzero */
                    s16 w = rd_s16(dec + zo + 8);
                    s16 h = rd_s16(dec + zo + 10);
                    if (w == 0 && h == 0) continue;
                    RdtCameraCutZone& cc = ov.camcuts[ov.n_camcuts++];
                    cc.type = dec[zo];
                    cc.cam_group = (u8)g;
                    cc.x = rd_s16(dec + zo + 4);
                    cc.z = rd_s16(dec + zo + 6);
                    cc.w = w;
                    cc.h = h;
                }
            }
            if (ov.n_camcuts > 0) found++;
        }
    }

    /* ─── SCD Script Walk ───
     *
     * The SCD region starts at ptr[5] with an offset table:
     *   u32[0] = offset to script[0] entry point (relative to scd_base)
     *   ...
     *   u32[N-1] = offset to script[N-1]
     * First u32 / 4 gives the script count (table size = first_offset bytes).
     * Each thread is decoded straight through to its end (scd_thread_end),
     * so both sides of every branch are visited once.
     */
    scan_section_placements(dec, dec_size, base, offs[5], meta_min, ov);
    if (ov.n_xforms > 0) found++;

    {
        size_t scd_base = offs[5];
        size_t scd_end = dec_size;
        for (int i = 0; i < 7; i++)
            if (offs[i] > scd_base && offs[i] < scd_end) scd_end = offs[i];
        if (scan_trigger_zones(dec, scd_base, scd_end, ov)) found++;
        if (scan_enemy_spawns(dec, scd_base, scd_end, base, dec_size, ov)) found++;
        scan_camera_views(dec, scd_base, scd_end, ov);

        const u8* scd = dec + scd_base;
        size_t scd_size = scd_end - scd_base;
        if (scd_size < 4) goto done;

        u32 first_off = rd_u32(scd);
        if (first_off < 4 || first_off > 0x1000 || first_off > scd_size || (first_off & 3) != 0)
            goto done;

        /* Thread table, read as the script view reads it (scd_disassemble) */
        u32 toffs[64];
        int n_threads = 0;
        for (int i = 0; i < 64 && (u32)i * 4 < first_off; i++) {
            u32 t = rd_u32(scd + i * 4);
            if (t < first_off || t >= scd_size) break;
            toffs[n_threads++] = t;
        }

        for (int ti = 0; ti < n_threads; ti++) {
            size_t end = scd_base + scd_thread_end(scd, scd_size, toffs, n_threads, toffs[ti]);
            size_t pc = scd_base + toffs[ti];
            while (pc < end) {
                u8 op = dec[pc];
                int adv = scd_inst_size(dec, end, pc);
                if (adv <= 0) break;

                /* ─── Extract spatial data from known opcodes ───
                   (0x23 and item-zone section placements are found by
                   scan_section_placements, and 0x28 zones by
                   scan_trigger_zones, not by this walk.) */

                /* 0x42: Object (20 bytes), handler 0x4284AD.  Fills a 0x7C-byte
                   object record: byte[1] = record index (0xFF: any free one),
                   byte[2] = object type (+0x27), byte[3] (+0x2F), position
                   x/y/z = s16 at +4/+6/+8 (+0x20..), byte[10] = model source
                   (2: room model, 3: model slot byte[11] & 15), u16 at +12
                   (+0x2A), u32 at +16 (+0x68). */
                if (op == 0x42 && pc + 20 <= dec_size && ov.n_enemies < RDT_MAX_OBJECTS) {
                    RdtEnemySpawn es;
                    es.type  = dec[pc + 2];
                    es.id    = dec[pc + 1];
                    es.flags = dec[pc + 3];
                    es.px    = rd_s16(dec + pc + 4);
                    es.py    = rd_s16(dec + pc + 6);
                    es.pz    = rd_s16(dec + pc + 8);
                    es.rot_y = rd_s16(dec + pc + 12);
                    /* the same object on both sides of a branch is kept once */
                    bool dup = false;
                    for (int k = 0; k < ov.n_enemies && !dup; k++) {
                        const RdtEnemySpawn& o = ov.enemies[k];
                        dup = o.type == es.type && o.id == es.id && o.flags == es.flags &&
                              o.px == es.px && o.py == es.py && o.pz == es.pz && o.rot_y == es.rot_y;
                    }
                    if (!dup) { ov.enemies[ov.n_enemies++] = es; found++; }
                }

                /* 0x3D: Fog/atmosphere effect (12 bytes) */
                if (op == 0x3D && pc + 12 <= dec_size && ov.n_fog < 8) {
                    RdtFogParams& fp = ov.fog[ov.n_fog++];
                    fp.type = dec[pc + 1];
                    fp.dist_near = rd_s16(dec + pc + 4);
                    fp.dist_far  = rd_s16(dec + pc + 6);
                    fp.r = dec[pc + 10];
                    fp.g = dec[pc + 11];
                    fp.b = dec[pc + 8];  /* byte order from decompile */
                }

                /* 0x3C: Dynamic light color (8 bytes) */
                if (op == 0x3C && pc + 8 <= dec_size && ov.n_light_cols < 64) {
                    RdtLightColor& lc = ov.light_cols[ov.n_light_cols++];
                    lc.light_idx = dec[pc + 1];
                    lc.component = dec[pc + 2];
                    lc.value     = rd_u16(dec + pc + 4);
                }

                /* 0x20: EMD character placement (24 bytes) */
                if (op == 0x20 && pc + 24 <= dec_size && ov.n_chars < 16) {
                    u32 mptr = rd_u32(dec + pc + 16);
                    u32 aptr = rd_u32(dec + pc + 20);
                    if (mptr >= base && aptr >= base) {
                        RdtCharPlace& cp = ov.chars[ov.n_chars++];
                        cp.slot     = dec[pc + 1];
                        cp.flags    = dec[pc + 2];
                        cp.type     = dec[pc + 3];
                        cp.anim_set = dec[pc + 4];
                        cp.px       = rd_s16(dec + pc + 8);
                        cp.py       = rd_s16(dec + pc + 10);
                        cp.pz       = rd_s16(dec + pc + 12);
                        cp.rot_y    = rd_s16(dec + pc + 14);
                        cp.mesh_ptr = mptr;
                        cp.anim_ptr = aptr;
                        found++;
                    }
                }

                /* 0x4C: Camera setup (32 bytes) */
                if (op == 0x4C && pc + 32 <= dec_size) {
                    RdtCameraEntry cam;
                    cam.eye_x = rd_s16(dec + pc + 2);
                    cam.eye_y = rd_s16(dec + pc + 4);
                    cam.eye_z = rd_s16(dec + pc + 6);
                    cam.tgt_x = rd_s16(dec + pc + 18);
                    cam.tgt_y = rd_s16(dec + pc + 20);
                    cam.tgt_z = rd_s16(dec + pc + 22);
                    /* +8 is the eye's transition mode, the first argument of the
                       eye setter (0x421F97 in the GOG exe): 0 cuts, anything
                       else moves toward the new eye.  Not a field of view. */
                    cam.eye_mode = (u16)rd_s16(dec + pc + 8);
                    cam.flags = 0;
                    if (add_camera(ov, cam)) found++;
                }

                /* 0x2E: Camera eye + target, short form of 0x4C (20 bytes).
                   Its handler (0x427418) feeds the same camera setters as
                   0x4C: eye x/y/z at +2/+4/+6 (sub_4220F7), target x/y/z
                   at +8/+10/+12 (sub_4221B1), with each one's transition
                   at +16/+18 (0 = cut straight to it). */
                if (op == 0x2E && pc + 20 <= dec_size) {
                    RdtCameraEntry cam;
                    cam.eye_x = rd_s16(dec + pc + 2);
                    cam.eye_y = rd_s16(dec + pc + 4);
                    cam.eye_z = rd_s16(dec + pc + 6);
                    cam.tgt_x = rd_s16(dec + pc + 8);
                    cam.tgt_y = rd_s16(dec + pc + 10);
                    cam.tgt_z = rd_s16(dec + pc + 12);
                    cam.eye_mode = 0;
                    cam.flags = 0;
                    if (add_camera(ov, cam)) found++;
                }

                /* 0x3A is not a light: across the 637 uses in the 148 rooms byte 1
                   is always 0 and byte 2 is 1-15, and the handler installs a
                   behaviour on the bound character.  Room lights come from ptr[0]
                   only. */

                /* 0x5B enemy spawns are found by scan_enemy_spawns */

                pc += (size_t)adv;
            }
        }
    }

done:
    return found > 0;
}

/* Zones and collision rects are flat XZ shapes with no height of their
   own, so the viewer draws each on the floor beneath it.  Taking the most
   common vertex height picked the ceiling in rooms like ST105, whose
   ceiling (raw Y -3000) has more vertices than its floor (raw Y 0).  Only
   upward-facing horizontal faces count as floor; points spread over each
   zone and rect vote for every floor height beneath them, so a big
   platform or yard outside the walkable area (ST613) does not win.  Rooms
   whose overlay covers no floor fall back to the height with the most
   floor area.

   That gives the room-wide floor.  A shape keeps it when it lies under at
   least a third of the shape's points, so a desk or counter inside a zone
   does not lift it.  Otherwise the shape is on another level (ST201's
   balcony, ST202's platforms) and takes the floor under most of its
   points, ties going to the height more shapes vote for. */
static bool xz_in_tri(f32 px, f32 pz, const MeshVert& a, const MeshVert& b,
                      const MeshVert& c)
{
    f32 d1 = (px - b.x) * (a.z - b.z) - (a.x - b.x) * (pz - b.z);
    f32 d2 = (px - c.x) * (b.z - c.z) - (b.x - c.x) * (pz - c.z);
    f32 d3 = (px - a.x) * (c.z - a.z) - (c.x - a.x) * (pz - a.z);
    bool neg = d1 < 0 || d2 < 0 || d3 < 0;
    bool pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

void overlay_floor_heights(const Mesh& mesh, RdtSceneOverlay& ov)
{
    ov.floor_y = 0.0f;
    for (int i = 0; i < 64; i++)  ov.zone_y[i] = 0.0f;
    for (int i = 0; i < 256; i++) ov.rect_y[i] = 0.0f;
    for (int i = 0; i < 128; i++) ov.camcut_y[i] = 0.0f;
    for (int i = 0; i < 32; i++)  ov.cam_thread_y[i] = 0.0f;
    if (mesh.tri_count <= 0) return;
    int* ftri = (int*)malloc((size_t)mesh.tri_count * sizeof(int));
    int* fh   = (int*)malloc((size_t)mesh.tri_count * sizeof(int));
    if (!ftri || !fh) { free(ftri); free(fh); return; }

    int h_y[256], h_votes[256], h_cnt[256];
    f32 h_area[256];
    u8  h_hit[256];
    int n_h = 0, n_floor = 0;

    /* Mesh verts have Y = -(raw PSX Y), so a face whose normal has +Y faces up */
    for (int fi = 0; fi < mesh.tri_count; fi++) {
        const MeshTri& t = mesh.tris[fi];
        if (t.alt) continue;
        const MeshVert& a = mesh.verts[t.idx[0]];
        const MeshVert& b = mesh.verts[t.idx[1]];
        const MeshVert& c = mesh.verts[t.idx[2]];
        if (fabsf(a.y - b.y) > 1.0f || fabsf(a.y - c.y) > 1.0f) continue;
        f32 ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
        if (ny <= 0.0f) continue;
        int y = (int)floorf(a.y + 0.5f);
        int hi = -1;
        for (int h = 0; h < n_h && hi < 0; h++)
            if (h_y[h] == y) hi = h;
        if (hi < 0) {
            if (n_h == 256) continue;
            hi = n_h++;
            h_y[hi] = y; h_area[hi] = 0.0f; h_votes[hi] = 0;
        }
        h_area[hi] += ny;
        ftri[n_floor] = fi; fh[n_floor] = hi; n_floor++;
    }

    /* 3x3 sample points across a quad (corners in drawing order);
       h_cnt[h] = how many of them have floor height h beneath */
    auto sample = [&](const f32 qx[4], const f32 qz[4]) {
        static const f32 s[3] = { 0.25f, 0.5f, 0.75f };
        memset(h_cnt, 0, (size_t)n_h * sizeof(int));
        for (int u = 0; u < 3; u++)
            for (int w = 0; w < 3; w++) {
                f32 x0 = qx[0] + (qx[1] - qx[0]) * s[u], z0 = qz[0] + (qz[1] - qz[0]) * s[u];
                f32 x1 = qx[3] + (qx[2] - qx[3]) * s[u], z1 = qz[3] + (qz[2] - qz[3]) * s[u];
                f32 px = x0 + (x1 - x0) * s[w], pz = z0 + (z1 - z0) * s[w];
                memset(h_hit, 0, (size_t)n_h);
                for (int k = 0; k < n_floor; k++) {
                    if (h_hit[fh[k]]) continue;
                    const MeshTri& t = mesh.tris[ftri[k]];
                    if (xz_in_tri(px, pz, mesh.verts[t.idx[0]], mesh.verts[t.idx[1]],
                                  mesh.verts[t.idx[2]])) {
                        h_hit[fh[k]] = 1;
                        h_cnt[fh[k]]++;
                    }
                }
            }
    };
    auto zone_quad = [&](int i, f32 qx[4], f32 qz[4]) {
        for (int c = 0; c < 4; c++) { qx[c] = -(f32)ov.zones[i].x[c]; qz[c] = (f32)ov.zones[i].z[c]; }
    };
    auto rect_quad = [](s16 x, s16 z, s16 w, s16 h, f32 qx[4], f32 qz[4]) {
        f32 x0 = -(f32)x, x1 = -(f32)(x + w), z0 = (f32)z, z1 = (f32)(z + h);
        qx[0] = x0; qx[1] = x1; qx[2] = x1; qx[3] = x0;
        qz[0] = z0; qz[1] = z0; qz[2] = z1; qz[3] = z1;
    };
    f32 qx[4], qz[4];
    for (int i = 0; i < ov.n_zones; i++) {
        zone_quad(i, qx, qz);
        sample(qx, qz);
        for (int h = 0; h < n_h; h++) h_votes[h] += h_cnt[h];
    }
    for (int i = 0; i < ov.n_collisions; i++) {
        const RdtCollisionRect& r = ov.collisions[i];
        rect_quad(r.x, r.z, (s16)r.w, (s16)r.h, qx, qz);
        sample(qx, qz);
        for (int h = 0; h < n_h; h++) h_votes[h] += h_cnt[h];
    }

    auto ranks_above = [&](int a, int b) {
        return h_votes[a] > h_votes[b] || (h_votes[a] == h_votes[b] && h_area[a] > h_area[b]);
    };
    int best = -1;
    for (int h = 0; h < n_h; h++)
        if (best < 0 || ranks_above(h, best)) best = h;
    if (best < 0) { free(ftri); free(fh); return; }
    ov.floor_y = (f32)h_y[best];

    auto pick = [&](const f32 px[4], const f32 pz[4]) -> f32 {
        sample(px, pz);
        if (h_cnt[best] >= 3) return ov.floor_y;
        int p = -1;
        for (int h = 0; h < n_h; h++)
            if (h_cnt[h] > 0 && (p < 0 || h_cnt[h] > h_cnt[p] ||
                                 (h_cnt[h] == h_cnt[p] && ranks_above(h, p)))) p = h;
        return p >= 0 ? (f32)h_y[p] : ov.floor_y;
    };
    for (int i = 0; i < ov.n_zones; i++) {
        zone_quad(i, qx, qz);
        ov.zone_y[i] = pick(qx, qz);
    }
    for (int i = 0; i < ov.n_collisions; i++) {
        const RdtCollisionRect& r = ov.collisions[i];
        rect_quad(r.x, r.z, (s16)r.w, (s16)r.h, qx, qz);
        ov.rect_y[i] = pick(qx, qz);
    }
    for (int i = 0; i < ov.n_camcuts; i++) {
        const RdtCameraCutZone& cc = ov.camcuts[i];
        rect_quad(cc.x, cc.z, cc.w, cc.h, qx, qz);
        ov.camcut_y[i] = pick(qx, qz);
    }
    for (int i = 0; i < ov.n_cam_threads; i++) {
        for (int c = 0; c < 4; c++) { qx[c] = -(f32)ov.cam_threads[i].x[c]; qz[c] = (f32)ov.cam_threads[i].z[c]; }
        ov.cam_thread_y[i] = pick(qx, qz);
    }
    free(ftri); free(fh);
}

/*═══════════════════════════════════════════════════════════════════
 *  Texture usage  -  which tpage/CLUT/UVs every textured face samples
 *═══════════════════════════════════════════════════════════════════*/
static void add_mesh_usage(const Mesh& mesh, TexFaceUse*& out, int& count, int& cap)
{
    for (int i = 0; i < mesh.tri_count; i++) {
        if (count == cap) {
            int ncap = cap ? cap * 2 : 1024;
            TexFaceUse* p = (TexFaceUse*)realloc(out, (size_t)ncap * sizeof(TexFaceUse));
            if (!p) return;
            out = p; cap = ncap;
        }
        const MeshTri& t = mesh.tris[i];
        TexFaceUse& u = out[count++];
        u.tpage = t.tpage;
        u.clut  = t.clut;
        memcpy(u.uv, t.uv, sizeof(u.uv));
    }
}

int collect_texture_usage(const DatArchive& archive, TexFaceUse** out)
{
    TexFaceUse* faces = 0;
    int count = 0, cap = 0;

    for (int i = 0; i < archive.count; i++) {
        const DatEntry& e = archive.entries[i];
        if (archive.is_dc2) {
            /* Dino Crisis 2: the package's models; rooms have no decoded mesh */
            Buffer dec;
            EmdModel model;
            if (e.type == DAT_LZSS0 && dat_entry_payload(e, dec) &&
                dc2_parse_model(dec.data, dec.size, e.load_base, model))
                add_mesh_usage(model.mesh, faces, count, cap);
            continue;
        }
        if (!(e.y & 0x8000)) continue;
        if (e.type != DAT_LZSS0 && !is_raw_rdt_entry(e)) continue;

        Buffer dec;
        if (!dat_entry_payload(e, dec) || dec.size < 0x20) continue;
        if (is_mips_code_dec(dec.data, dec.size)) continue;
        u32 base = ((u32)(e.y & 0x7FFF) << 16) | (u32)e.x | 0x80000000u;

        RdtLayout layout;
        bool has_layout = parse_rdt_layout(dec.data, dec.size, base, layout);
        if (has_layout && layout.section_count > 0) {
            Mesh mesh;
            if (parse_rdt_scene_dec(dec.data, dec.size, base, mesh))
                add_mesh_usage(mesh, faces, count, cap);
        }
        if (has_layout && layout.emd_count > 0) {
            for (int k = 0; k < layout.emd_count && k < RDT_MAX_EMDS; k++) {
                EmdModel model;
                if (parse_emd_model_dec(dec.data, dec.size, e.y, e.x, model,
                                        (int)layout.emds[k].offset))
                    add_mesh_usage(model.mesh, faces, count, cap);
            }
        }
        if (!has_layout || (layout.section_count == 0 && layout.emd_count == 0)) {
            Mesh mesh;
            if (is_door_mesh_entry_dec(dec.data, dec.size, e.y)) {
                if (parse_door_mesh_dec(dec.data, dec.size, base, mesh))
                    add_mesh_usage(mesh, faces, count, cap);
            } else if (is_standalone_emd_dec(dec.data, dec.size, base)) {
                if (parse_standalone_emd_mesh_dec(dec.data, dec.size, base, mesh))
                    add_mesh_usage(mesh, faces, count, cap);
            }
        }
    }

    *out = faces;
    return count;
}

/*═══════════════════════════════════════════════════════════════════
 *  Camera shots (ptr[3] descriptors)
 *
 *  Each frame the game calls the handler for the active descriptor's
 *  type (table at 0x645FA8), which builds a target and an eye and passes
 *  them to sub_423EB7 in that order.  Angles are 4096 to a turn; the
 *  rotations are libgte's RotMatrixY (rows c,0,s / 0,1,0 / -s,0,c) and
 *  RotMatrixX (1,0,0 / 0,c,-s / 0,s,c).  The player position is the one
 *  at +0x20 of the player work (y = floor height, up is negative).
 *═══════════════════════════════════════════════════════════════════*/
static f32 cam_rad(f32 a) { return a * 6.2831853f / 4096.0f; }

/* Ry(yaw) * Rx(pitch) * (0,0,d), as sub_5F3700 + ApplyMatrixSV */
static void cam_orbit(f32 yaw, f32 pitch, f32 d, f32 out[3])
{
    f32 sy = sinf(cam_rad(yaw)), cy = cosf(cam_rad(yaw));
    f32 sp = sinf(cam_rad(pitch)), cp = cosf(cam_rad(pitch));
    out[0] = sy * cp * d;
    out[1] = -sp * d;
    out[2] = cy * cp * d;
}

static f32 cam_atan2(f32 y, f32 x) { return atan2f(y, x) * 4096.0f / 6.2831853f; }

int rdt_camera_shot(const u8* d, const f32 p[3], f32 eye[3], f32 tgt[3])
{
    #define S16(o) ((f32)rd_s16(d + (o)))
    #define U16(o) ((f32)rd_u16(d + (o)))
    f32 v[3];
    switch (d[0]) {
    case 1: case 34: case 35:      /* fixed: target +0x2C, eye +0x34 */
        tgt[0] = S16(0x2C); tgt[1] = S16(0x2E); tgt[2] = S16(0x30);
        eye[0] = S16(0x34); eye[1] = S16(0x36); eye[2] = S16(0x38);
        return RDT_SHOT_FIXED;
    case 3:                        /* fixed eye watching the player */
        tgt[0] = p[0]; tgt[1] = p[1] - S16(0x34); tgt[2] = p[2];
        eye[0] = S16(0x2C); eye[1] = S16(0x2E); eye[2] = S16(0x30);
        return RDT_SHOT_TRACKING;
    case 4: case 31: {             /* orbit a fixed target, facing the player */
        tgt[0] = S16(0x2C); tgt[1] = S16(0x2E); tgt[2] = S16(0x30);
        f32 yaw = cam_atan2(tgt[0] - p[0], tgt[2] - p[2]) + 0x800;
        if (d[0] == 4) {
            cam_orbit(yaw, cam_atan2(-S16(0x36), U16(0x34)), -S16(0x34), v);
        } else {
            f32 dx = p[0] - tgt[0], dy = p[1] - tgt[1], dz = p[2] - tgt[2];
            cam_orbit(yaw, cam_atan2(S16(0x36), sqrtf(dx * dx + dy * dy + dz * dz)), S16(0x34), v);
        }
        for (int k = 0; k < 3; k++) eye[k] = tgt[k] + v[k];
        return RDT_SHOT_TRACKING;
    }
    case 7:                        /* chase at a fixed yaw */
        tgt[0] = p[0]; tgt[1] = p[1] - S16(0x2C); tgt[2] = p[2];
        cam_orbit(S16(0x2A), 0, -S16(0x30), v);
        eye[0] = p[0] + v[0]; eye[1] = p[1] - S16(0x2E); eye[2] = p[2] + v[2];
        return RDT_SHOT_TRACKING;
    case 8: case 9:                /* slide along z (8) or x (9) with the player */
        if (d[0] == 8) { tgt[0] = S16(0x2C); tgt[2] = p[2]; }
        else           { tgt[0] = p[0];      tgt[2] = S16(0x2C); }
        tgt[1] = p[1] - S16(0x32);
        cam_orbit(-S16(0x30), 0, -S16(0x2E), v);
        eye[0] = tgt[0] + v[0]; eye[1] = tgt[1] - S16(0x34); eye[2] = tgt[2] + v[2];
        return RDT_SHOT_TRACKING;
    case 17: case 18: {            /* side-on: one axis fixed, the other follows */
        tgt[0] = p[0]; tgt[1] = p[1] - S16(0x32); tgt[2] = p[2];
        eye[1] = tgt[1] - S16(0x34);
        int a = rd_u16(d + 0x30) & 0xFFF;
        if (d[0] == 17) {
            eye[0] = S16(0x2C);
            eye[2] = (a > 0x400 && a < 0xC00) ? p[2] + S16(0x2E) : p[2] - S16(0x2E);
        } else {
            eye[0] = (rd_u16(d + 0x30) & 0x800) ? p[0] + S16(0x2E) : p[0] - S16(0x2E);
            eye[2] = S16(0x2C);
        }
        return RDT_SHOT_TRACKING;
    }
    case 26: {                     /* eye on the rail A-B, in line with target and player */
        tgt[0] = S16(0x2C); tgt[1] = S16(0x2E); tgt[2] = S16(0x30);
        f32 a[3] = { S16(0x34), S16(0x36), S16(0x38) }, b[3] = { S16(0x3C), S16(0x3E), S16(0x40) };
        f32 den = (b[0] - a[0]) * (tgt[2] - p[2]) - (b[2] - a[2]) * (tgt[0] - p[0]);
        f32 t = den != 0 ? ((tgt[0] - p[0]) * (a[2] - p[2]) - (tgt[2] - p[2]) * (a[0] - p[0])) / den : 0;
        for (int k = 0; k < 3; k++) eye[k] = a[k] + (b[k] - a[k]) * t;
        return RDT_SHOT_TRACKING;
    }
    }
    #undef S16
    #undef U16
    return RDT_SHOT_NONE;   /* 0 and unused types; 10/11/19-21/32/33 shift the previous shot */
}

#include "emd_glb.inc"
