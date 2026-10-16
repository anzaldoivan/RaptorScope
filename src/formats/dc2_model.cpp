/*═══════════════════════════════════════════════════════════════════
 *  RaptorScope  -  Dino Crisis 2 Models
 *
 *  DCM character, enemy and weapon models (E*.DAT, WEP_*.DAT, ...) and
 *  the texture a model's header selects.  The layout is in dc2.h.
 *
 *  Sources: dinorand tools/DinoCrisis.Model (DcmModel.cs, DcmVariant.cs,
 *  DcmLocator.cs, ObjExporter.cs, DcmTextureDecoder.cs), K127.
 *═══════════════════════════════════════════════════════════════════*/
#include "formats/dc2.h"
#include <stdlib.h>
#include <string.h>

/*─── Header ─────────────────────────────────────────────────────*/

struct DcmHead {
    u32 vtx, nrm, tri, quad;    /* blob offsets of the four pools */
    int n_verts, n_tris, n_quads, n_parts;
    u16 tpage, clut;
};

/* Absolute pointer to blob offset; false when it is below the load address */
static bool resolve(u32 ptr, u32 base, u32& off)
{
    if (ptr < base) return false;
    off = ptr - base;
    return true;
}

/* Read and check the header.  A blob is taken as a model only when every
   structural rule holds, the same ones dinorand's DcmModel.Validate
   applies; 133 entries of the PC game pass them (K127).
     - the vertex pool starts right after the part array
     - vertex, normal, triangle and quad pools follow in that order, the
       first two with the same number of 8-byte entries
     - the triangle pool is exactly triangles x 12 bytes
     - part 0 is the root; every other part's parent comes before it
     - the vertex count is 3 x the sum of the parts' byte +7 (each part
       owns that many vertices of the pool, in part order)
     - the parts' triangle and quad ranges are back to back and add up to
       the header totals
     - every face index is inside the vertex pool
   The header is at the start of the blob, so there is no scan to guard
   and dinorand's limit of 4096 on a part's offset is left out: the root
   of E40.DAT's model stands 4544 above the ground. */
static bool read_head(const u8* dec, size_t dec_size, u32 base, DcmHead& h)
{
    if (!dec || dec_size < DC2_DCM_HEADER + DC2_DCM_PART || dec_size > 0x7FFFFFFFu)
        return false;

    u32 pv = rd_u32(dec), pn = rd_u32(dec + 4), pt = rd_u32(dec + 8), pq = rd_u32(dec + 12);
    h.n_tris  = rd_u16(dec + 0x10);
    h.n_quads = rd_u16(dec + 0x12);
    u32 parts = rd_u32(dec + 0x14);
    h.tpage   = rd_u16(dec + 0x18);
    h.clut    = rd_u16(dec + 0x1A);
    if (parts < 1 || parts > DC2_DCM_MAX_PARTS) return false;
    h.n_parts = (int)parts;

    if (!(pv <= pn && pn <= pt && pt <= pq)) return false;
    if (!resolve(pv, base, h.vtx) || !resolve(pn, base, h.nrm) ||
        !resolve(pt, base, h.tri) || !resolve(pq, base, h.quad)) return false;
    if (h.vtx != DC2_DCM_HEADER + parts * DC2_DCM_PART) return false;
    if ((pn - pv) % 8 || (pt - pn) % 8 || pn - pv != pt - pn) return false;
    h.n_verts = (int)((pn - pv) / 8);
    if (pq - pt != (u32)h.n_tris * 12) return false;
    if ((size_t)h.quad + (size_t)h.n_quads * 16 > dec_size) return false;

    /* Parts */
    int tri_sum = 0, quad_sum = 0, vert_sum = 0;
    u32 next_tri = pt, next_quad = pq;
    for (int i = 0; i < h.n_parts; i++) {
        const u8* p = dec + DC2_DCM_HEADER + i * DC2_DCM_PART;
        int parent = (s8)p[6];
        if (i == 0 ? parent >= 0 : (parent < 0 || parent >= i)) return false;
        if (rd_u32(p + 8) != next_tri || rd_u32(p + 12) != next_quad) return false;
        next_tri  += (u32)rd_u16(p + 0x10) * 12;
        next_quad += (u32)rd_u16(p + 0x12) * 16;
        tri_sum  += rd_u16(p + 0x10);
        quad_sum += rd_u16(p + 0x12);
        vert_sum += p[7] * 3;
    }
    if (tri_sum != h.n_tris || quad_sum != h.n_quads || vert_sum != h.n_verts) return false;

    /* Face indices */
    for (int i = 0; i < h.n_tris; i++)
        for (int k = 0; k < 3; k++)
            if (rd_u16(dec + h.tri + i * 12 + k * 2) >= h.n_verts) return false;
    for (int i = 0; i < h.n_quads; i++)
        for (int k = 0; k < 4; k++)
            if (rd_u16(dec + h.quad + i * 16 + k * 2) >= h.n_verts) return false;
    return true;
}

bool dc2_model_probe(const u8* dec, size_t dec_size, u32 base, Dc2ModelInfo& info)
{
    DcmHead h;
    if (!read_head(dec, dec_size, base, h)) return false;
    info.parts = h.n_parts;
    info.verts = h.n_verts;
    info.tris  = h.n_tris;
    info.quads = h.n_quads;
    info.tpage = h.tpage;
    info.clut  = h.clut;
    return true;
}

/*─── Model ──────────────────────────────────────────────────────*/

/* Build the viewer's character model in the bind pose.

   Vertices are stored relative to a bone.  The vertex pool is laid out
   part by part: part i owns the next 3 x (its byte +7) vertices, and its
   bone places them, whichever part's face uses them.  A face on a joint
   reaches into the neighbouring part's vertices, which is what closes the
   seams: placing a face's corners by the face's own part instead (as
   dinorand's ObjExporter does for DC2) pulls E00.DAT's raptor apart at
   every joint.  The vertex's fourth word repeats the owning part's number
   in most models.

   Viewer coordinates negate all three axes, as for DC1 characters.  A
   quad is a PS1 strip, (v0, v1, v2) and (v1, v3, v2): of E00.DAT's and
   WEP_P000.DAT's 320 quads, 303 are only consistent that way round.
   tpage and clut come from the header and are the same for every face. */
bool dc2_parse_model(const u8* dec, size_t dec_size, u32 base, EmdModel& model)
{
    model.valid = false;
    DcmHead h;
    if (!read_head(dec, dec_size, base, h)) return false;

    int n_corners = h.n_tris * 3 + h.n_quads * 4;
    int n_faces   = h.n_tris + h.n_quads * 2;

    free(model.parts_info); free(model.pool); free(model.vert_src);
    model.parts_info = (EmdModel::PartInfo*)calloc(h.n_parts, sizeof(EmdModel::PartInfo));
    model.pool       = (EmdModel::PoolVert*)calloc(h.n_verts ? h.n_verts : 1, sizeof(EmdModel::PoolVert));
    model.vert_src   = (EmdModel::VertSrc*)calloc(n_corners ? n_corners : 1, sizeof(EmdModel::VertSrc));
    if (!model.parts_info || !model.pool || !model.vert_src) return false;
    model.parts_count = h.n_parts;
    model.pool_count = h.n_verts;
    model.vert_src_count = n_corners;
    model.base_addr = base;
    model.norm_pool_off = h.nrm;
    model.mesh.alloc(n_corners, n_faces);

    /* Bones: each part's offset is relative to its parent.  Then the
       vertex pool, each part's run tagged with that part. */
    f32 abs_pos[DC2_DCM_MAX_PARTS][3];
    memset(&model.skeleton, 0, sizeof(model.skeleton));
    model.skeleton.bone_count = h.n_parts;
    int pool_at = 0;
    for (int i = 0; i < h.n_parts; i++) {
        const u8* p = dec + DC2_DCM_HEADER + i * DC2_DCM_PART;
        EmdModel::PartInfo& pi = model.parts_info[i];
        pi.bx = rd_s16(p); pi.by = rd_s16(p + 2); pi.bz = rd_s16(p + 4);
        pi.parent = (i == 0) ? -1 : (s8)p[6];
        for (int a = 0; a < 3; a++)
            abs_pos[i][a] = (f32)rd_s16(p + a * 2) + (pi.parent < 0 ? 0.0f : abs_pos[pi.parent][a]);
        EmdBone& b = model.skeleton.bones[i];
        b.bx = pi.bx; b.by = pi.by; b.bz = pi.bz;
        b.parent = (u8)(pi.parent < 0 ? 0xFF : pi.parent);
        b.n_tris = rd_u16(p + 0x10);
        b.n_quads = rd_u16(p + 0x12);
        b.vert_start = pool_at;
        b.vert_count = p[7] * 3;
        for (int a = 0; a < 3; a++)
            model.skeleton.abs_pos[i][a] = -abs_pos[i][a];
        for (int k = 0; k < b.vert_count; k++, pool_at++) {
            const u8* v = dec + h.vtx + (size_t)pool_at * 8;
            EmdModel::PoolVert& pv = model.pool[pool_at];
            pv.x = rd_s16(v); pv.y = rd_s16(v + 2); pv.z = rd_s16(v + 4);
            pv.bone_idx = (u16)i;
        }
    }

    /* Faces, part by part */
    int nv = 0, nt = 0;
    size_t tri_off = h.tri, quad_off = h.quad;
    for (int part = 0; part < h.n_parts; part++) {
        const EmdBone& b = model.skeleton.bones[part];
        for (int f = 0; f < b.n_tris + b.n_quads; f++) {
            bool is_quad = f >= b.n_tris;
            int corners = is_quad ? 4 : 3;
            const u8* rec = dec + (is_quad ? quad_off : tri_off);
            const u8* uv = rec + corners * 2;
            if (is_quad) quad_off += 16; else tri_off += 12;

            int first = nv;
            for (int k = 0; k < corners; k++) {
                int vi = rd_u16(rec + k * 2);
                const EmdModel::PoolVert& pv = model.pool[vi];
                const u8* n = dec + h.nrm + (size_t)vi * 8;
                model.vert_src[nv].pool_idx = vi;
                model.vert_src[nv].is_mirror = false;
                model.vert_src[nv].dest_bone = pv.bone_idx;
                MeshVert& mv = model.mesh.verts[nv++];
                mv.x = -((f32)pv.x + abs_pos[pv.bone_idx][0]);
                mv.y = -((f32)pv.y + abs_pos[pv.bone_idx][1]);
                mv.z = -((f32)pv.z + abs_pos[pv.bone_idx][2]);
                mv.nx = -(f32)rd_s16(n);
                mv.ny = -(f32)rd_s16(n + 2);
                mv.nz = -(f32)rd_s16(n + 4);
            }

            static const int order[2][3] = { { 0, 1, 2 }, { 1, 3, 2 } };
            for (int t = 0; t < (is_quad ? 2 : 1); t++) {
                MeshTri& mt = model.mesh.tris[nt++];
                mt.color = RGBA8(128, 128, 128, 255);
                mt.tpage = h.tpage;
                mt.clut  = h.clut;
                for (int k = 0; k < 3; k++) {
                    int c = order[t][k];
                    mt.idx[k]   = (u32)(first + c);
                    mt.uv[k][0] = uv[c * 2];
                    mt.uv[k][1] = uv[c * 2 + 1];
                }
            }
        }
    }
    model.valid = true;
    return true;
}

/*─── Texture selection ──────────────────────────────────────────*/

/* The header's tpage gives the texture page (x = low 4 bits x 64
   halfwords, y = bit 4 x 256) and colour depth, and its clut the palette
   position (x = low 6 bits x 16, y = the rest), exactly as a DC1 face's
   do.  The texture and palette are the entries whose VRAM rectangle
   holds that position.  E00.DAT has no such entries: an enemy's skin is
   in the .TEX file of the same name, an ordinary package (E00.TEX is one
   128 x 256 halfword texture at (896, 256) and one palette at (0, 511)). */
bool dc2_model_texture(const DatArchive& archive, u16 tpage, u16 clut,
                       int& tex_entry, int& pal_entry)
{
    int page_x = (tpage & 0xF) * 64, page_y = ((tpage >> 4) & 1) * 256;
    int clut_x = (clut & 0x3F) * 16, clut_y = clut >> 6;
    tex_entry = pal_entry = -1;
    for (int i = 0; i < archive.count; i++) {
        const DatEntry& e = archive.entries[i];
        bool is_tex = e.type == DAT_TEXTURE || e.type == DAT_LZSS1;
        if (!is_tex && e.type != DAT_PALETTE) continue;
        int px = is_tex ? page_x : clut_x, py = is_tex ? page_y : clut_y;
        if (px < e.x || px >= e.x + e.w || py < e.y || py >= e.y + e.h) continue;
        if (is_tex && tex_entry < 0) tex_entry = i;
        if (!is_tex && pal_entry < 0) pal_entry = i;
    }
    int depth = (tpage >> 7) & 3;   /* 0 = 4bpp, 1 = 8bpp, 2 = 16bpp */
    return tex_entry >= 0 && (depth == 2 || pal_entry >= 0);
}
