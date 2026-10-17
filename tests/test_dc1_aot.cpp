/* Synthetic DC1 AOT parser/classifier tests.  No game files are read. */
#include "formats/mesh.h"
#include "formats/scd.h"
#include <stdio.h>
#include <string.h>
#include <vector>

static int g_checks = 0, g_failed = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void put16(std::vector<u8>& v, size_t p, u16 n)
{
    v[p] = (u8)n; v[p + 1] = (u8)(n >> 8);
}

static void put32(std::vector<u8>& v, size_t p, u32 n)
{
    v[p] = (u8)n; v[p + 1] = (u8)(n >> 8);
    v[p + 2] = (u8)(n >> 16); v[p + 3] = (u8)(n >> 24);
}

static std::vector<u8> aot(int action, int slot = 1)
{
    int len = action >= 0 && action < 12 ? g_scd_op28_sizes[action] : 32;
    std::vector<u8> r((size_t)len, 0);
    r[0] = 0x28; r[1] = (u8)slot; r[2] = (u8)action; r[3] = 1;
    for (int c = 0; c < 4; c++) {
        put16(r, 4 + c * 4, (u16)(100 + c * 10));
        put16(r, 6 + c * 4, (u16)(200 + c * 10));
    }
    if (len > 23) { r[20] = (u8)action; r[23] = 1; }
    return r;
}

static void append(std::vector<u8>& dst, const std::vector<u8>& src)
{
    dst.insert(dst.end(), src.begin(), src.end());
}

static void finish(std::vector<u8>& thread)
{
    const u8 end[] = { 0x04, 0, 0, 0 };
    thread.insert(thread.end(), end, end + 4);
}

static std::vector<u8> script(const std::vector<std::vector<u8> >& threads)
{
    size_t table = threads.size() * 4;
    std::vector<u8> out(table, 0);
    for (size_t i = 0; i < threads.size(); i++) {
        put32(out, i * 4, (u32)out.size());
        append(out, threads[i]);
    }
    return out;
}

static const RdtFloorZone* find_action(const RdtSceneOverlay& ov, int action, int nth = 0)
{
    for (int i = 0; i < ov.n_zones; i++)
        if (ov.zones[i].kind == RDT_ZONE_TRIGGER && ov.zones[i].target_index == action)
            if (nth-- == 0) return &ov.zones[i];
    return 0;
}

static void test_lengths_and_names()
{
    std::vector<u8> t;
    for (int action = 0; action < 12; action++) append(t, aot(action, action + 1));
    finish(t);
    std::vector<std::vector<u8> > threads(1, t);
    std::vector<u8> scd = script(threads);
    RdtSceneOverlay ov;
    CHECK(dc1_parse_aot_zones(scd.data(), scd.size(), 0x4000, ov) == 12);
    for (int action = 0; action < 12; action++) {
        const RdtFloorZone* z = find_action(ov, action);
        CHECK(z && z->record_length == g_scd_op28_sizes[action]);
        CHECK(z && z->raw_size == g_scd_op28_sizes[action] && !z->malformed);
    }
    CHECK(strcmp(dc1_aot_action_name(0), "Door / room transition") == 0);
    CHECK(strcmp(dc1_aot_action_name(2), "Set flag on entry") == 0);
    CHECK(strcmp(dc1_aot_action_name(3), "Run event subroutine") == 0);
    CHECK(strcmp(dc1_aot_action_name(4), "Item pickup") == 0);
    CHECK(strcmp(dc1_aot_action_name(12), "Unknown action") == 0);
}

static void test_owners_duplicates_and_payloads()
{
    std::vector<u8> set_a = aot(2), set_b = set_a;
    set_a[28] = 3; set_a[29] = 32; set_a[30] = 1;
    set_b[28] = 3; set_b[29] = 33; set_b[30] = 1;
    std::vector<u8> t0; append(t0, set_a); append(t0, set_a); append(t0, set_b); finish(t0);
    std::vector<u8> t1; append(t1, set_a); finish(t1);
    std::vector<std::vector<u8> > threads; threads.push_back(t0); threads.push_back(t1);
    std::vector<u8> scd = script(threads);
    RdtSceneOverlay ov;
    CHECK(dc1_parse_aot_zones(scd.data(), scd.size(), 0x1000, ov) == 3);
    const RdtFloorZone* a = find_action(ov, 2, 0);
    const RdtFloorZone* b = find_action(ov, 2, 1);
    const RdtFloorZone* c = find_action(ov, 2, 2);
    CHECK(a && b && c);
    CHECK(a->owner_subroutine == 0 && b->owner_subroutine == 0 && c->owner_subroutine == 1);
    CHECK(a->x[0] == b->x[0] && a->z[0] == b->z[0] && a->raw[29] != b->raw[29]);
    CHECK(c->source_offset >= 0x1000 && c->action_confidence == DC1_AOT_CONFIRMED);
    char text[160];
    dc1_aot_format_record(a->raw, a->raw_size, text, sizeof(text));
    CHECK(strstr(text, "flag 3:32 = 1") != 0);
}

static void test_malformed_and_unknown()
{
    std::vector<u8> short7 = aot(7); short7.resize(24);
    std::vector<u8> t0 = short7;
    std::vector<u8> t1; finish(t1);
    std::vector<std::vector<u8> > threads; threads.push_back(t0); threads.push_back(t1);
    std::vector<u8> scd = script(threads);
    RdtSceneOverlay ov;
    CHECK(dc1_parse_aot_zones(scd.data(), scd.size(), 0, ov) == 1);
    const RdtFloorZone* z = find_action(ov, 7);
    CHECK(z && z->malformed && z->raw_size == 24 && z->record_length == 52);
    CHECK(z && z->action_confidence == DC1_AOT_UNKNOWN);

    std::vector<u8> t2 = aot(12); finish(t2);
    std::vector<std::vector<u8> > one(1, t2);
    scd = script(one);
    RdtSceneOverlay ov2;
    CHECK(dc1_parse_aot_zones(scd.data(), scd.size(), 0, ov2) == 1);
    z = find_action(ov2, 12);
    CHECK(z && z->malformed && z->raw_size == 32 && z->record_length == 0);
    char text[128];
    dc1_aot_format_record(z->raw, z->raw_size, text, sizeof(text));
    CHECK(strstr(text, "Unknown action (type 12)") != 0 && strstr(text, "raw:") != 0);
    char details[512];
    dc1_aot_format_details(*z, details, sizeof(details));
    CHECK(strstr(details, "length 32/unknown") != 0);
}

static std::vector<u8> action3(int target)
{
    std::vector<u8> r = aot(3);
    r[28] = 4;
    put16(r, 30, (u16)target);
    return r;
}

static void test_classifier()
{
    std::vector<std::vector<u8> > t(15);
    for (int target = 1; target <= 6; target++) append(t[0], action3(target));
    const int followed[] = { 7, 9, 11, 13, 14 };
    for (int i = 0; i < 5; i++) append(t[0], action3(followed[i]));
    finish(t[0]);

    const u8 ddk[] = { 0x58, 0, 3, 0, 0, 0, 0, 0 };
    t[1].insert(t[1].end(), ddk, ddk + sizeof(ddk)); finish(t[1]);

    const u8 cut_on[] = { 0x25, 2, 2, 1 }, cut_off[] = { 0x25, 2, 2, 0 };
    t[2].insert(t[2].end(), cut_on, cut_on + 4);
    std::vector<u8> enemy(24, 0); enemy[0] = 0x20; append(t[2], enemy);
    t[2].insert(t[2].end(), cut_off, cut_off + 4); finish(t[2]);

    append(t[3], aot(4)); finish(t[3]);
    const u8 screen[] = { 0x58, 0, 4, 0, 0, 0, 0, 0 };
    t[4].insert(t[4].end(), screen, screen + sizeof(screen)); finish(t[4]);
    std::vector<u8> enemy2(20, 0); enemy2[0] = 0x59; append(t[5], enemy2); finish(t[5]);
    finish(t[6]);

    /* Subs reached through GOSUB (0x0F), a task start (0x01) and jumps (0x05) */
    const u8 gosub8[] = { 0x0F, 0, 8, 0 }, start10[] = { 0x01, 0xFF, 10, 0 };
    const u8 jump12[] = { 0x05, 0, 12, 0 }, jump11[] = { 0x05, 0, 11, 0 };
    t[7].insert(t[7].end(), gosub8, gosub8 + 4); finish(t[7]);
    t[8].insert(t[8].end(), ddk, ddk + sizeof(ddk)); finish(t[8]);
    t[9].insert(t[9].end(), start10, start10 + 4); finish(t[9]);
    append(t[10], aot(4)); finish(t[10]);
    t[11].insert(t[11].end(), jump12, jump12 + 4); finish(t[11]);
    t[12].insert(t[12].end(), jump11, jump11 + 4); finish(t[12]);
    std::vector<u8> human(24, 0); human[0] = 0x20; human[2] = 1;
    append(t[13], human); finish(t[13]);
    std::vector<u8> raptor(24, 0); raptor[0] = 0x20; raptor[2] = 2;
    append(t[14], raptor); finish(t[14]);

    std::vector<u8> scd = script(t);
    RdtSceneOverlay ov;
    CHECK(dc1_parse_aot_zones(scd.data(), scd.size(), 0x200, ov) >= 6);
    CHECK(find_action(ov, 3, 0)->behavior_flags == DC1_AOT_BEHAVIOR_DDK);
    CHECK(find_action(ov, 3, 1)->behavior_flags ==
          (DC1_AOT_BEHAVIOR_CUTSCENE | DC1_AOT_BEHAVIOR_ENCOUNTER));
    CHECK(find_action(ov, 3, 2)->behavior_flags == DC1_AOT_BEHAVIOR_ITEM);
    CHECK(find_action(ov, 3, 3)->behavior_flags == DC1_AOT_BEHAVIOR_INTERFACE);
    CHECK(find_action(ov, 3, 4)->behavior_flags == DC1_AOT_BEHAVIOR_ENCOUNTER);
    CHECK(find_action(ov, 3, 5)->behavior_flags == 0);
    CHECK(find_action(ov, 3, 0)->behavior_confidence == DC1_AOT_DERIVED);
    CHECK(find_action(ov, 3, 5)->behavior_confidence == DC1_AOT_UNKNOWN);
    CHECK(find_action(ov, 4)->owner_subroutine == 3 && find_action(ov, 4)->owner_targeted);
    char provenance[320];
    dc1_aot_format_label(*find_action(ov, 4), provenance, sizeof(provenance));
    CHECK(strstr(provenance, "targeted by another event") != 0);
    char label[256];
    dc1_aot_format_label(*find_action(ov, 3, 1), label, sizeof(label));
    CHECK(strstr(label, "Cutscene + encounter (class 0) [derived]") != 0);

    CHECK(find_action(ov, 3, 6)->behavior_flags == DC1_AOT_BEHAVIOR_DDK);
    CHECK(find_action(ov, 3, 7)->behavior_flags == DC1_AOT_BEHAVIOR_ITEM);
    CHECK(find_action(ov, 3, 8)->behavior_flags == 0);
    CHECK(find_action(ov, 3, 9)->behavior_flags == 0);
    CHECK(find_action(ov, 3, 10)->behavior_flags == DC1_AOT_BEHAVIOR_ENCOUNTER);
    CHECK(find_action(ov, 3, 10)->enemy_classes == (1 << 2));
    dc1_aot_format_label(*find_action(ov, 3, 10), label, sizeof(label));
    CHECK(strstr(label, "encounter (Velociraptor) [derived]") != 0);
}

static void test_item_names()
{
    std::vector<u8> r = aot(4);
    r[0x1C] = 0x69; put16(r, 0x1E, 1);
    char text[160];
    dc1_aot_format_record(r.data(), r.size(), text, sizeof(text));
    CHECK(strstr(text, "item 0x69 DDK Code Disc H x1") != 0);
    r[0x1C] = 0xFF;
    dc1_aot_format_record(r.data(), r.size(), text, sizeof(text));
    CHECK(strstr(text, "item set at run time") != 0);
    r[0x1C] = 0x70;
    dc1_aot_format_record(r.data(), r.size(), text, sizeof(text));
    CHECK(strstr(text, "item 0x70 x1") != 0);
}

static void test_walk_continues_and_limits()
{
    /* (a) an unknown sub-type steps 32 and the next record is still seen */
    std::vector<u8> t = aot(13); append(t, aot(2, 5)); finish(t);
    std::vector<u8> scd = script(std::vector<std::vector<u8> >(1, t));
    RdtSceneOverlay ov;
    CHECK(dc1_parse_aot_zones(scd.data(), scd.size(), 0, ov) == 2);
    const RdtFloorZone* z = find_action(ov, 13);
    CHECK(z && z->malformed && find_action(ov, 2) && !find_action(ov, 2)->malformed);

    /* (c) more than 64 AOTs: the zone list is capped, the total is not */
    std::vector<u8> big;
    for (int i = 0; i < 70; i++) { std::vector<u8> r = aot(2); r[28] = (u8)i; append(big, r); }
    finish(big);
    scd = script(std::vector<std::vector<u8> >(1, big));
    RdtSceneOverlay ov2;
    dc1_parse_aot_zones(scd.data(), scd.size(), 0, ov2);
    CHECK(ov2.aot_overflow && ov2.n_aots_total == 70 && ov2.n_zones == 64);

    /* (d) actions 8 and 10 are confirmed from their handlers; real records */
    CHECK(strcmp(dc1_aot_action_name(8), "Emergency box") == 0);
    CHECK(strcmp(dc1_aot_action_name(10), "Fingerprint-taking spot") == 0);
    CHECK(strcmp(dc1_aot_action_name(9), "No-op (unused)") == 0);
    std::vector<u8> t8 = aot(8); finish(t8);
    scd = script(std::vector<std::vector<u8> >(1, t8));
    RdtSceneOverlay ov3;
    dc1_parse_aot_zones(scd.data(), scd.size(), 0, ov3);
    z = find_action(ov3, 8);
    CHECK(z && z->action_confidence == DC1_AOT_CONFIRMED);
    char details[512];
    dc1_aot_format_details(*z, details, sizeof(details));
    CHECK(strstr(details, "basis: native action handler") != 0);
    {
        const char* st104 = "28040800500990F3D00F90F3D00F00F0500900F008030001000000000D010000";
        const char* st103 = "280B0A0060F67005C0FA000640FB5002E0F6C0010A0100010000000004000000";
        u8 r8[32], r10[32];
        for (int i = 0; i < 32; i++) {
            unsigned a, b;
            sscanf(st104 + i * 2, "%2x", &a); r8[i] = (u8)a;
            sscanf(st103 + i * 2, "%2x", &b); r10[i] = (u8)b;
        }
        char text[128];
        dc1_aot_format_record(r8, 32, text, sizeof(text));
        CHECK(strstr(text, "box 13 (yellow), 1 plug") != 0 && !strstr(text, "raw:"));
        dc1_aot_format_record(r10, 32, text, sizeof(text));
        CHECK(strstr(text, "print 4") != 0 && !strstr(text, "raw:"));
    }

    /* (e) formatters always NUL-terminate, in a tiny and a near-exact buffer */
    char tiny[8];
    memset(tiny, 'x', sizeof(tiny));
    dc1_aot_format_details(*z, tiny, sizeof(tiny));
    CHECK(memchr(tiny, 0, sizeof(tiny)) != 0);
    size_t full = strlen(details);
    for (size_t n = full - 1; n <= full + 1; n++) {
        char buf[512];
        memset(buf, 'x', sizeof(buf));
        dc1_aot_format_details(*z, buf, n);
        CHECK(memchr(buf, 0, n) != 0);
    }
}

int main()
{
    test_lengths_and_names();
    test_owners_duplicates_and_payloads();
    test_malformed_and_unknown();
    test_classifier();
    test_item_names();
    test_walk_continues_and_limits();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
