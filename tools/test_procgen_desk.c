/* Host harness: run the REAL procgen over many seeds and prove that every
 * generated level places the desk console.
 *
 * Build+run:  cc -Ish_src -o /tmp/pgdesk tools/test_procgen_desk.c \
 *                sh_src/procgen.c && /tmp/pgdesk 5000
 *
 * Why it exists: the desk scan required an all-open 3x3 footprint AND a wall
 * to back onto, which cannot both be true, so place_pvms silently placed
 * nothing in every generated level. Nothing on the console side was broken,
 * so nothing looked broken — the set piece was simply never there. This
 * harness turns "should be guaranteed" into a number. Stubs stand in for the engine
 * side; the only behaviour that must be faithful is what the desk scan
 * actually reads — the grid, standup occupancy, and the exit path. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#define memset __builtin_memset
#include "raycast.h"
#include "procgen.h"

/* ---- engine state procgen writes into ---- */
player_t player;
uint8_t  world_map[MAP_H][MAP_W];
uint8_t  ceil_h[MAP_H][MAP_W];
partition_t partitions[NUM_PARTITIONS_MAX];
uint8_t  partition_style[NUM_PARTITIONS_MAX];
uint8_t  partition_height[NUM_PARTITIONS_MAX];
uint8_t  partition_decor[NUM_PARTITIONS_MAX];
int      num_partitions;
uint8_t  pedge_w[MAP_H][MAP_W + 1];
uint8_t  pedge_n[MAP_H + 1][MAP_W];
int      g_pedge_any;
int      g_lobby_ceiling;
const struct cm_light_s *g_map_lights; uint16_t g_map_n_lights;
const struct cm_dark_s  *g_map_dark;   uint8_t  g_map_n_dark;
int      g_lowceil_active;
int      num_decals;

/* ---- standup table: the parts the desk scan consults ---- */
#define MAXS 64
static struct { int x, y; uint8_t kind, desk; } sus[MAXS];
static int nsus;
int  num_standups;

void raycast_add_standup(fx_t x, fx_t y, uint8_t facing, uint8_t kind) {
    (void)facing;
    if (nsus < MAXS) {
        sus[nsus].x = FX_INT(x); sus[nsus].y = FX_INT(y);
        sus[nsus].kind = kind;   sus[nsus].desk = 0;
        nsus++;
    }
    num_standups = nsus;
}
void standups_clear(void) { nsus = 0; num_standups = 0; }
void raycast_standup_make_desk(void) { if (nsus) sus[nsus - 1].desk = 1; }
int  raycast_standup_in_cell(int x, int y) {
    for (int i = 0; i < nsus; i++) if (sus[i].x == x && sus[i].y == y) return 1;
    return 0;
}

/* The exit corridor: procgen stamps the door, then the desk scan avoids the
 * protected path. Reproduce the real bit-array so the rejection matches. */
extern uint32_t exit_path_bits[MAP_H];
uint32_t exit_path_bits[MAP_H];
int  raycast_exit_path_cell(int x, int y) {
    return (int)((exit_path_bits[y] >> x) & 1u);
}
void raycast_place_exit_door(void) {}
void raycast_place_exit_hole(void) {}
void raycast_place_outlets(int t) { (void)t; }
void raycast_add_dark_room(int a,int b,int c,int d){(void)a;(void)b;(void)c;(void)d;}
void raycast_stamp_partition_edges(void) {}
void pedge_clear(void) {}
void ceil_h_clear(void) { memset(ceil_h, CEIL_H_FULL, sizeof ceil_h); }
void ceil_h_add_run_h(int cx,int cy,int dx,int dy,int len,int h){
    for (int i=0;i<len;i++){int x=cx+dx*i,y=cy+dy*i;
        if((unsigned)x<MAP_W&&(unsigned)y<MAP_H) ceil_h[y][x]=(uint8_t)h;}
}

int main(int argc, char **argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 5000;
    int no_desk = 0, no_pvm = 0, worst_seed = -1;
    for (int s = 0; s < n; s++) {
        nsus = 0; num_standups = 0;
        memset(sus, 0, sizeof sus);
        memset(exit_path_bits, 0, sizeof exit_path_bits);
        procgen_params_default();
        procgen_run((uint32_t)s + 1);
        int desks = 0, pvms = 0;
        for (int i = 0; i < nsus; i++) {
            if (sus[i].kind != PVM_ASSET_KIND) continue;
            pvms++;
            if (sus[i].desk) desks++;
        }
        if (!pvms)  { no_pvm++;  if (worst_seed < 0) worst_seed = s + 1; }
        if (!desks) { no_desk++; if (worst_seed < 0) worst_seed = s + 1; }
    }
    printf("seeds: %d\n", n);
    printf("levels with NO pvm at all : %d\n", no_pvm);
    printf("levels with NO desk console: %d\n", no_desk);
    if (worst_seed >= 0) printf("first failing seed: %d\n", worst_seed);
    return (no_pvm || no_desk) ? 1 : 0;
}
