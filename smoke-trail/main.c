/* ==============================================================================
 * WARHAMMER 40,000 GLADIUS: RELICS OF WAR - THUNDERHAWK KINETIC FLIGHT ENGINE
 *
 * Features:
 *   - 3D-shaded Space Marine Thunderhawk Gunship (Macragge & Ultramarine Blue)
 *   - Authentic White Ultima Symbol ('U') & Armored Amber-Gold Cockpit Canopy
 *   - Dorsal Turbo-Laser (#FFD700) & Mars-Pattern Twin Turbofans (#00E5FF Glow)
 *   - Dynamic Kinetic Flight: Bank Left/Right, Rise on Forward Speed,
 *     Hover & Dive on Left Mouse Button, Sudden Stop Nose Flare & Settling
 *   - Aerodynamic Twin Wingtip & Nacelle Vapor Contrails (Cyan-White Condensation)
 *   - Continuous Nacelle Engine Audio Thread (Deep 58Hz/116Hz Turbine Rumble)
 *   - Adaptive Socket Polling (~28 Hz Idle Backoff) & 60 FPS Capped Render Tick
 * ============================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <glob.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <pthread.h>
#include <linux/input.h>
#include <alsa/asoundlib.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <cairo.h>

#define MAX_VAPOR_PARTICLES 450
#define VAPOR_STAMP_SIZE 64

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* --- Vapor Particle State --- */
typedef struct {
    double x, y;
    double vx, vy;
    double length;
    double angle;
    double age, lifetime;
    double alpha0;
    double r, g, b;
    double radius;
    gboolean active;
} VaporParticle;

typedef struct {
    double x, y;
    double length;
    double angle;
    double radius;
    double alpha;
    double r, g, b;
} RenderVapor;

/* --- Thunderhawk Kinetic Flight State --- */
typedef struct {
    double x, y;
    double heading;         /* in degrees, 0 = north/up */
    double target_heading;
    double bank;            /* -1.0 (hard left) to +1.0 (hard right) */
    double target_bank;
    double pitch_flare;     /* nose pitch on sudden deceleration */
    double scale;
    int is_diving;          /* 1 when Left Mouse Button is depressed */
    int is_moving;
    double speed;
} ThunderhawkState;

typedef struct {
    GtkWidget *window;
    GtkWidget *drawing_area;
    GdkMonitor *monitor;
    int mon_x, mon_y, mon_w, mon_h;
} MonitorOverlay;

/* Global state */
static VaporParticle vapor_pool[MAX_VAPOR_PARTICLES];
static int vapor_head = 0;
static pthread_mutex_t v_mutex = PTHREAD_MUTEX_INITIALIZER;

static RenderVapor render_vapor_snap[MAX_VAPOR_PARTICLES];
static int render_vapor_count = 0;
static ThunderhawkState th_state = { 0, 0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0, 0, 0.0 };
static ThunderhawkState th_render_snap = { 0, 0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0, 0, 0.0 };
static pthread_mutex_t snap_mutex = PTHREAD_MUTEX_INITIALIZER;

static volatile gboolean is_running = TRUE;
static volatile gboolean is_animating = FALSE;
static volatile gboolean needs_wake = FALSE;
static volatile float audio_target_volume = 0.04f;
static volatile float audio_current_volume = 0.0f;

static GList *overlay_list = NULL;
static GtkApplication *global_app = NULL;
static guint tick_source_id = 0;
static cairo_surface_t *vapor_stamp = NULL;

static double rand_range(double min, double max) {
    return min + (double)rand() / (double)RAND_MAX * (max - min);
}

/* --- Pre-cached High-Performance Crystalline Vapor Stamp --- */
static void init_vapor_stamp(void) {
    vapor_stamp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, VAPOR_STAMP_SIZE, VAPOR_STAMP_SIZE);
    cairo_t *cr = cairo_create(vapor_stamp);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    double mid = VAPOR_STAMP_SIZE / 2.0;
    cairo_pattern_t *pat = cairo_pattern_create_radial(mid, mid, 1.0, mid, mid, mid);
    cairo_pattern_add_color_stop_rgba(pat, 0.00, 1.0, 1.0, 1.0, 0.95);
    cairo_pattern_add_color_stop_rgba(pat, 0.35, 0.88, 0.96, 1.0, 0.70);
    cairo_pattern_add_color_stop_rgba(pat, 0.70, 0.65, 0.88, 1.0, 0.28);
    cairo_pattern_add_color_stop_rgba(pat, 1.00, 0.45, 0.75, 1.0, 0.00);

    cairo_set_source(cr, pat);
    cairo_arc(cr, mid, mid, mid, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);
    cairo_destroy(cr);
}

/* --- Nacelle Exhaust Sound Generation via ALSA --- */
static void *audio_thread_func(void *arg) {
    (void)arg;
    snd_pcm_t *pcm = NULL;

    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        return NULL;
    }

    /* 48 kHz, mono, 16-bit signed PCM, low-latency buffer (50ms) */
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, 48000, 1, 50000) < 0) {
        snd_pcm_close(pcm);
        return NULL;
    }

    short pcm_buffer[480]; /* 10 ms audio chunks */
    double phase_f0 = 0.0;
    double phase_f1 = 0.0;
    double phase_f2 = 0.0;

    const double inc_f0 = 2.0 * M_PI * 58.0 / 48000.0;  /* 58 Hz primary drone */
    const double inc_f1 = 2.0 * M_PI * 116.0 / 48000.0; /* 116 Hz 2nd harmonic */
    const double inc_f2 = 2.0 * M_PI * 232.0 / 48000.0; /* 232 Hz nacelle whine */

    while (is_running) {
        /* Smooth volume interpolation */
        float target = audio_target_volume;
        audio_current_volume += (target - audio_current_volume) * 0.12f;

        if (audio_current_volume < 0.002f) {
            memset(pcm_buffer, 0, sizeof(pcm_buffer));
            snd_pcm_sframes_t frames = snd_pcm_writei(pcm, pcm_buffer, 480);
            if (frames < 0) {
                snd_pcm_recover(pcm, frames, 0);
            }
            continue;
        }

        for (int i = 0; i < 480; i++) {
            phase_f0 += inc_f0;
            phase_f1 += inc_f1;
            phase_f2 += inc_f2;
            if (phase_f0 > 2.0 * M_PI) phase_f0 -= 2.0 * M_PI;
            if (phase_f1 > 2.0 * M_PI) phase_f1 -= 2.0 * M_PI;
            if (phase_f2 > 2.0 * M_PI) phase_f2 -= 2.0 * M_PI;

            double sample = (0.50 * sin(phase_f0) +
                             0.32 * sin(phase_f1 + 0.4) +
                             0.18 * sin(phase_f2 + 0.9)) * (double)audio_current_volume;

            /* Soft clipping protection */
            if (sample > 1.0) sample = 1.0;
            if (sample < -1.0) sample = -1.0;

            pcm_buffer[i] = (short)(sample * 32767.0);
        }

        snd_pcm_sframes_t frames = snd_pcm_writei(pcm, pcm_buffer, 480);
        if (frames < 0) {
            snd_pcm_recover(pcm, frames, 0);
        }
    }

    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    return NULL;
}

/* --- Mouse Button Detection Thread (/dev/input/by-id) --- */
static void *mouse_button_thread_func(void *arg) {
    (void)arg;
    glob_t glob_result;
    memset(&glob_result, 0, sizeof(glob_result));

    /* Search for mouse event device nodes */
    int fds[16];
    int fd_count = 0;

    if (glob("/dev/input/by-id/*-event-mouse", 0, NULL, &glob_result) == 0) {
        for (size_t i = 0; i < glob_result.gl_pathc && fd_count < 16; i++) {
            int fd = open(glob_result.gl_pathv[i], O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                fds[fd_count++] = fd;
            }
        }
    }
    globfree(&glob_result);

    /* Fallback search if by-id was empty */
    if (fd_count == 0) {
        for (int i = 0; i < 32 && fd_count < 16; i++) {
            char path[64];
            snprintf(path, sizeof(path), "/dev/input/event%d", i);
            int fd = open(path, O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                /* Probe for mouse keys */
                unsigned long keybit[(KEY_MAX + 31) / 32];
                memset(keybit, 0, sizeof(keybit));
                if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) >= 0) {
                    if (keybit[BTN_LEFT / 32] & (1UL << (BTN_LEFT % 32))) {
                        fds[fd_count++] = fd;
                        continue;
                    }
                }
                close(fd);
            }
        }
    }

    struct input_event ev;
    while (is_running) {
        int had_event = 0;
        for (int i = 0; i < fd_count; i++) {
            while (read(fds[i], &ev, sizeof(ev)) > 0) {
                had_event = 1;
                if (ev.type == EV_KEY && ev.code == BTN_LEFT) {
                    pthread_mutex_lock(&v_mutex);
                    th_state.is_diving = (ev.value != 0);
                    needs_wake = TRUE;
                    pthread_mutex_unlock(&v_mutex);
                }
            }
        }
        if (!had_event) {
            usleep(10000); /* 10 ms sleep when idle */
        }
    }

    for (int i = 0; i < fd_count; i++) {
        close(fds[i]);
    }
    return NULL;
}

/* --- Spawn Aerodynamic Wingtip and Nacelle Vapor Contrails --- */
static void spawn_vapor_streamer(double x, double y, double vx, double vy, double radius, double alpha, double angle) {
    VaporParticle *p = &vapor_pool[vapor_head];
    vapor_head = (vapor_head + 1) % MAX_VAPOR_PARTICLES;

    p->x = x;
    p->y = y;
    p->vx = -vx * 0.15 + rand_range(-3.0, 3.0);
    p->vy = -vy * 0.15 + rand_range(-3.0, 3.0);
    p->length = rand_range(12.0, 24.0);
    p->angle = angle;
    p->age = 0.0;
    p->lifetime = rand_range(0.25, 0.45); /* Rapid atmospheric dissipation */
    p->alpha0 = alpha;
    p->radius = radius;

    /* Icy crystalline atmospheric color stop */
    p->r = 0.92;
    p->g = 0.97;
    p->b = 1.00;
    p->active = TRUE;
}

/* --- Cairo Drawing: 3D-Shaded Space Marine Thunderhawk Gunship --- */
static void render_cairo_thunderhawk(cairo_t *cr, double cx, double cy, double heading_deg, double bank, double pitch_flare, double scale, int is_diving) {
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_rotate(cr, heading_deg * M_PI / 180.0);

    /* 3D Perspective Scaling: Bank foreshortens wings, Pitch alters fuselage ratio */
    double sx = scale * (1.0 - fabs(bank) * 0.16);
    double sy = scale * (1.0 + pitch_flare * 0.14);
    if (is_diving) {
        sx *= 0.85;
        sy *= 0.85;
    }
    cairo_scale(cr, sx, sy);
    cairo_translate(cr, -50.0, -50.0);

    /* 1. Dynamic Drop Shadow (Expands with flight altitude) */
    double shadow_dx = -bank * 4.0;
    double shadow_dy = is_diving ? 3.0 : (7.0 + scale * 5.0);
    double shadow_alpha = is_diving ? 0.65 : 0.42;

    cairo_save(cr);
    cairo_translate(cr, shadow_dx, shadow_dy);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, shadow_alpha);
    cairo_move_to(cr, 50, 10);
    cairo_line_to(cr, 46, 24); cairo_line_to(cr, 30, 26); cairo_line_to(cr, 30, 30); cairo_line_to(cr, 46, 29);
    cairo_line_to(cr, 47, 45); cairo_line_to(cr, 10, 50); cairo_line_to(cr, 10, 62); cairo_line_to(cr, 47, 54);
    cairo_line_to(cr, 47, 82); cairo_line_to(cr, 24, 85); cairo_line_to(cr, 24, 90); cairo_line_to(cr, 44, 90);
    cairo_line_to(cr, 56, 90); cairo_line_to(cr, 76, 90); cairo_line_to(cr, 76, 85); cairo_line_to(cr, 53, 82);
    cairo_line_to(cr, 53, 54); cairo_line_to(cr, 90, 62); cairo_line_to(cr, 90, 50); cairo_line_to(cr, 53, 45);
    cairo_line_to(cr, 54, 29); cairo_line_to(cr, 70, 30); cairo_line_to(cr, 70, 26); cairo_line_to(cr, 54, 24);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_restore(cr);

    /* 2. Primary Swept Delta Wings with Macragge & Ultramarine Blue Gradients */
    /* Left Wing */
    cairo_pattern_t *pat_wl = cairo_pattern_create_linear(10, 50, 47, 54);
    cairo_pattern_add_color_stop_rgb(pat_wl, 0.0, 0.11, 0.31, 0.85); /* #1D4ED8 */
    cairo_pattern_add_color_stop_rgb(pat_wl, 0.6, 0.06, 0.32, 0.73); /* Macragge Blue #0F52BA */
    cairo_pattern_add_color_stop_rgb(pat_wl, 1.0, 0.04, 0.15, 0.32); /* Ultramarine Shadow #092552 */
    cairo_set_source(cr, pat_wl);
    cairo_move_to(cr, 47, 45); cairo_line_to(cr, 10, 50); cairo_line_to(cr, 10, 56); cairo_line_to(cr, 47, 54);
    cairo_close_path(cr);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
    cairo_set_line_width(cr, 1.1);
    cairo_stroke(cr);
    cairo_pattern_destroy(pat_wl);

    /* Right Wing */
    cairo_pattern_t *pat_wr = cairo_pattern_create_linear(90, 50, 53, 54);
    cairo_pattern_add_color_stop_rgb(pat_wr, 0.0, 0.15, 0.39, 0.92);
    cairo_pattern_add_color_stop_rgb(pat_wr, 0.6, 0.06, 0.32, 0.73);
    cairo_pattern_add_color_stop_rgb(pat_wr, 1.0, 0.04, 0.15, 0.32);
    cairo_set_source(cr, pat_wr);
    cairo_move_to(cr, 53, 45); cairo_line_to(cr, 90, 50); cairo_line_to(cr, 90, 56); cairo_line_to(cr, 53, 54);
    cairo_close_path(cr);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
    cairo_set_line_width(cr, 1.1);
    cairo_stroke(cr);
    cairo_pattern_destroy(pat_wr);

    /* Kinetic Trailing-Edge Wing Flaps (Ailerons Deflected based on Bank) */
    double flap_l_def = bank * 3.5;
    double flap_r_def = -bank * 3.5;

    cairo_set_source_rgb(cr, 0.04, 0.16, 0.37);
    cairo_move_to(cr, 10, 56); cairo_line_to(cr, 10, 62 + flap_l_def); cairo_line_to(cr, 22, 60 + flap_l_def); cairo_line_to(cr, 47, 54);
    cairo_close_path(cr); cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85); cairo_set_line_width(cr, 0.9); cairo_stroke(cr);

    cairo_set_source_rgb(cr, 0.04, 0.16, 0.37);
    cairo_move_to(cr, 90, 56); cairo_line_to(cr, 90, 62 + flap_r_def); cairo_line_to(cr, 78, 60 + flap_r_def); cairo_line_to(cr, 53, 54);
    cairo_close_path(cr); cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85); cairo_set_line_width(cr, 0.9); cairo_stroke(cr);

    /* Canards & Tail Stabilizers */
    cairo_set_source_rgb(cr, 0.06, 0.32, 0.73);
    cairo_move_to(cr, 46, 24); cairo_line_to(cr, 30, 26); cairo_line_to(cr, 30, 30); cairo_line_to(cr, 46, 29);
    cairo_close_path(cr); cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85); cairo_stroke(cr);

    cairo_set_source_rgb(cr, 0.06, 0.32, 0.73);
    cairo_move_to(cr, 54, 24); cairo_line_to(cr, 70, 26); cairo_line_to(cr, 70, 30); cairo_line_to(cr, 54, 29);
    cairo_close_path(cr); cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85); cairo_stroke(cr);

    cairo_move_to(cr, 47, 82); cairo_line_to(cr, 24, 85); cairo_line_to(cr, 24, 90); cairo_line_to(cr, 47, 88);
    cairo_close_path(cr); cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85); cairo_stroke(cr);

    cairo_move_to(cr, 53, 82); cairo_line_to(cr, 76, 85); cairo_line_to(cr, 76, 90); cairo_line_to(cr, 53, 88);
    cairo_close_path(cr); cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85); cairo_stroke(cr);

    /* Twin Mars-Pattern Heavy Turbofans */
    cairo_set_source_rgb(cr, 0.09, 0.09, 0.09);
    cairo_rectangle(cr, 33.5, 44, 8, 26); cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9); cairo_set_line_width(cr, 1.0); cairo_stroke(cr);

    cairo_set_source_rgb(cr, 0.09, 0.09, 0.09);
    cairo_rectangle(cr, 58.5, 44, 8, 26); cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9); cairo_set_line_width(cr, 1.0); cairo_stroke(cr);

    /* Plasma Exhaust Glowing Nozzles */
    cairo_set_source_rgb(cr, 0.0, 0.90, 1.0);
    cairo_rectangle(cr, 34.5, 67.5, 6, 4.5); cairo_fill(cr);
    cairo_rectangle(cr, 59.5, 67.5, 6, 4.5); cairo_fill(cr);

    /* Main Armored Fuselage Backbone (3D Gradient Shading) */
    cairo_pattern_t *pat_fuse = cairo_pattern_create_linear(44, 50, 56, 50);
    cairo_pattern_add_color_stop_rgb(pat_fuse, 0.00, 0.03, 0.13, 0.29); /* Ultramarine Shadow */
    cairo_pattern_add_color_stop_rgb(pat_fuse, 0.25, 0.06, 0.32, 0.73); /* Macragge Blue */
    cairo_pattern_add_color_stop_rgb(pat_fuse, 0.50, 0.15, 0.39, 0.92); /* Specular Center Spine */
    cairo_pattern_add_color_stop_rgb(pat_fuse, 0.75, 0.06, 0.32, 0.73);
    cairo_pattern_add_color_stop_rgb(pat_fuse, 1.00, 0.02, 0.09, 0.19);
    cairo_set_source(cr, pat_fuse);

    cairo_move_to(cr, 50, 10);
    cairo_line_to(cr, 54, 18); cairo_line_to(cr, 54, 32); cairo_line_to(cr, 53, 42); cairo_line_to(cr, 53, 82);
    cairo_line_to(cr, 56, 85); cairo_line_to(cr, 56, 90); cairo_line_to(cr, 44, 90); cairo_line_to(cr, 44, 85);
    cairo_line_to(cr, 47, 82); cairo_line_to(cr, 47, 42); cairo_line_to(cr, 46, 32); cairo_line_to(cr, 46, 18);
    cairo_close_path(cr);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);
    cairo_pattern_destroy(pat_fuse);

    /* Armored Amber-Gold Cockpit Canopy Glass */
    cairo_set_source_rgb(cr, 1.0, 0.88, 0.51);
    cairo_move_to(cr, 50, 15); cairo_line_to(cr, 52.8, 21); cairo_line_to(cr, 47.2, 21);
    cairo_close_path(cr); cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.90, 0.66, 0.24);
    cairo_move_to(cr, 47.2, 21); cairo_line_to(cr, 52.8, 21); cairo_line_to(cr, 52.5, 27); cairo_line_to(cr, 47.5, 27);
    cairo_close_path(cr); cairo_fill(cr);

    /* White Ultramarines Ultima Heraldry Symbol ('U') on Dorsal Hull */
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_set_line_width(cr, 1.4);
    cairo_move_to(cr, 47.5, 33); cairo_line_to(cr, 47.5, 37);
    cairo_curve_to(cr, 47.5, 39.5, 52.5, 39.5, 52.5, 37);
    cairo_line_to(cr, 52.5, 33);
    cairo_stroke(cr);
    /* Serif Bars */
    cairo_move_to(cr, 46.5, 33); cairo_line_to(cr, 48.5, 33); cairo_stroke(cr);
    cairo_move_to(cr, 51.5, 33); cairo_line_to(cr, 53.5, 33); cairo_stroke(cr);

    /* Dorsal Mounted Turbo-Laser Weapon Barrel (Imperial Gold) */
    cairo_set_source_rgb(cr, 1.0, 0.84, 0.0);
    cairo_rectangle(cr, 48.5, 21, 3, 30); cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9); cairo_set_line_width(cr, 0.6); cairo_stroke(cr);

    /* Diving Attack Reticle Flash when clicked */
    if (is_diving) {
        cairo_set_source_rgba(cr, 1.0, 0.84, 0.0, 0.85);
        cairo_arc(cr, 50, 10, 8.0, 0, 2 * M_PI);
        cairo_set_line_width(cr, 1.5);
        cairo_stroke(cr);
    }

    cairo_restore(cr);
}

/* --- Layer Shell Draw Function --- */
static void draw_flight_func(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    (void)area;
    MonitorOverlay *ov = (MonitorOverlay*)user_data;

    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    RenderVapor local_vapor[MAX_VAPOR_PARTICLES];
    int v_count = 0;
    ThunderhawkState th;

    pthread_mutex_lock(&snap_mutex);
    v_count = render_vapor_count;
    if (v_count > 0) {
        memcpy(local_vapor, render_vapor_snap, v_count * sizeof(RenderVapor));
    }
    th = th_render_snap;
    pthread_mutex_unlock(&snap_mutex);

    if (ov->monitor) {
        GdkRectangle geom;
        gdk_monitor_get_geometry(ov->monitor, &geom);
        ov->mon_x = geom.x;
        ov->mon_y = geom.y;
        ov->mon_w = geom.width;
        ov->mon_h = geom.height;
    }

    /* 1. Render Crystalline Vapor Contrails */
    double half_stamp = VAPOR_STAMP_SIZE / 2.0;
    for (int i = 0; i < v_count; i++) {
        RenderVapor *vp = &local_vapor[i];
        double lx = vp->x - ov->mon_x;
        double ly = vp->y - ov->mon_y;
        double r = vp->radius;

        if (lx + r < 0 || lx - r > width || ly + r < 0 || ly - r > height) {
            continue;
        }

        double sc = r / half_stamp;
        cairo_save(cr);
        cairo_translate(cr, lx, ly);
        cairo_scale(cr, sc, sc);
        cairo_translate(cr, -half_stamp, -half_stamp);
        cairo_set_source_surface(cr, vapor_stamp, 0, 0);
        cairo_paint_with_alpha(cr, vp->alpha);
        cairo_restore(cr);
    }

    /* 2. Render Thunderhawk Gunship on Overlay */
    double th_lx = th.x - ov->mon_x;
    double th_ly = th.y - ov->mon_y;

    if (th_lx >= -60 && th_lx <= width + 60 && th_ly >= -60 && th_ly <= height + 60) {
        render_cairo_thunderhawk(cr, th_lx, th_ly, th.heading, th.bank, th.pitch_flare, th.scale, th.is_diving);
    }
}

static gint64 last_frame_time = 0;
static gboolean on_frame_tick(gpointer user_data);

static gboolean wake_animation(gpointer user_data) {
    (void)user_data;
    if (!is_animating) {
        is_animating = TRUE;
        last_frame_time = g_get_monotonic_time();
        if (tick_source_id == 0) {
            tick_source_id = g_timeout_add(16, on_frame_tick, NULL); /* 60 FPS cap */
        }
    }
    return G_SOURCE_REMOVE;
}

static gboolean on_frame_tick(gpointer user_data) {
    (void)user_data;
    gint64 now = g_get_monotonic_time();
    if (last_frame_time == 0) last_frame_time = now;
    double dt = (double)(now - last_frame_time) / 1000000.0;
    last_frame_time = now;
    if (dt > 0.05) dt = 0.05;

    RenderVapor next_snap[MAX_VAPOR_PARTICLES];
    int snap_count = 0;

    pthread_mutex_lock(&v_mutex);
    int active_vapor = 0;
    for (int i = 0; i < MAX_VAPOR_PARTICLES; i++) {
        VaporParticle *p = &vapor_pool[i];
        if (!p->active) continue;

        p->age += dt;
        if (p->age >= p->lifetime) {
            p->active = FALSE;
            continue;
        }

        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->vx *= (1.0 - 0.40 * dt);
        p->vy *= (1.0 - 0.40 * dt);

        double progress = p->age / p->lifetime;
        if (progress < 1.0 && snap_count < MAX_VAPOR_PARTICLES) {
            RenderVapor *rv = &next_snap[snap_count++];
            rv->x = p->x;
            rv->y = p->y;
            rv->radius = p->radius * (1.0 + progress * 0.8);
            rv->alpha = p->alpha0 * pow(1.0 - progress, 1.2);
            rv->r = p->r;
            rv->g = p->g;
            rv->b = p->b;
        }
        active_vapor++;
    }

    /* Damped harmonic settling calculation for nose pitch flare */
    if (fabs(th_state.pitch_flare) > 0.005) {
        th_state.pitch_flare *= (1.0 - 7.5 * dt);
        needs_wake = TRUE;
    } else {
        th_state.pitch_flare = 0.0;
    }

    /* Bank angle recovery toward neutral equilibrium */
    if (!th_state.is_moving && fabs(th_state.bank) > 0.005) {
        th_state.bank *= (1.0 - 8.0 * dt);
        needs_wake = TRUE;
    }

    ThunderhawkState current_th = th_state;
    gboolean stop_animating = (active_vapor == 0 && !needs_wake && th_state.pitch_flare == 0.0);
    needs_wake = FALSE;
    pthread_mutex_unlock(&v_mutex);

    pthread_mutex_lock(&snap_mutex);
    render_vapor_count = snap_count;
    if (snap_count > 0) {
        memcpy(render_vapor_snap, next_snap, snap_count * sizeof(RenderVapor));
    }
    th_render_snap = current_th;
    pthread_mutex_unlock(&snap_mutex);

    if (stop_animating) {
        is_animating = FALSE;
        tick_source_id = 0;
        for (GList *l = overlay_list; l != NULL; l = l->next) {
            MonitorOverlay *ov = (MonitorOverlay*)l->data;
            gtk_widget_queue_draw(ov->drawing_area);
        }
        return G_SOURCE_REMOVE;
    }

    for (GList *l = overlay_list; l != NULL; l = l->next) {
        MonitorOverlay *ov = (MonitorOverlay*)l->data;
        gtk_widget_queue_draw(ov->drawing_area);
    }

    return G_SOURCE_CONTINUE;
}

static void on_map(GtkWidget *widget, gpointer data) {
    (void)data;
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(widget));
    if (surface) {
        cairo_region_t *empty = cairo_region_create();
        gdk_surface_set_input_region(surface, empty);
        cairo_region_destroy(empty);
    }
}

static gboolean quit_app_idle(gpointer data) {
    (void)data;
    if (global_app) {
        g_application_quit(G_APPLICATION(global_app));
    }
    return G_SOURCE_REMOVE;
}

/* --- Background Poller Thread: Hyprland Socket & Aerodynamics --- */
static void *poll_thread_func(void *arg) {
    (void)arg;
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!runtime || !sig) {
        return NULL;
    }

    char sock_path[512];
    snprintf(sock_path, sizeof(sock_path), "%s/hypr/%s/.socket.sock", runtime, sig);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, sock_path, strlen(sock_path) + 1);

    int prev_x = -1, prev_y = -1;
    double last_speed = 0.0;
    int idle_streak = 0;
    useconds_t sleep_us = 8000;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 40000 };

    while (is_running) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) {
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                const char *cmd = "cursorpos";
                if (write(fd, cmd, strlen(cmd)) > 0) {
                    char buf[64];
                    ssize_t n = read(fd, buf, sizeof(buf) - 1);
                    if (n > 0) {
                        buf[n] = '\0';
                        int cx, cy;
                        if (sscanf(buf, "%d, %d", &cx, &cy) == 2) {
                            if (prev_x != -1 && (cx != prev_x || cy != prev_y)) {
                                double dx = cx - prev_x;
                                double dy = cy - prev_y;
                                double dist = sqrt(dx * dx + dy * dy);

                                if (dist > 1.5) {
                                    /* 1. Calculate Target Heading */
                                    double target_hdg = atan2(dy, dx) * 180.0 / M_PI + 90.0;
                                    double hdg_diff = target_hdg - th_state.heading;
                                    while (hdg_diff < -180.0) hdg_diff += 360.0;
                                    while (hdg_diff > 180.0) hdg_diff -= 360.0;

                                    /* Aerodynamic lag smoothing */
                                    th_state.heading += hdg_diff * 0.26;

                                    /* 2. Bank Left/Right depending on turning vector */
                                    double bank_target = hdg_diff * 0.045;
                                    if (bank_target > 0.6) bank_target = 0.6;
                                    if (bank_target < -0.6) bank_target = -0.6;
                                    th_state.bank += (bank_target - th_state.bank) * 0.28;

                                    /* 3. Rise when going forward (dy < 0 or speed > 6) */
                                    double target_scale = 1.0;
                                    if (dy < -1.0 || dist > 6.0) {
                                        target_scale = 1.15; /* Cruising altitude lift */
                                    }
                                    th_state.scale += (target_scale - th_state.scale) * 0.20;

                                    th_state.x = cx;
                                    th_state.y = cy;
                                    th_state.is_moving = 1;
                                    th_state.speed = dist;
                                    last_speed = dist;

                                    /* 4. Audio: Modulate nacelle rumble volume with speed */
                                    float vol = 0.06f + (float)(dist / 35.0f) * 0.26f;
                                    if (vol > 0.35f) vol = 0.35f;
                                    audio_target_volume = vol;

                                    /* 5. Spawn Twin Wingtip & Twin Nacelle Vapor Contrails */
                                    pthread_mutex_lock(&v_mutex);
                                    double rad_hdg = th_state.heading * M_PI / 180.0;
                                    double cos_h = cos(rad_hdg);
                                    double sin_h = sin(rad_hdg);

                                    /* Wingtip coordinates: (-38, +4) and (+38, +4) */
                                    double wx_l = cx + (cos_h * -38.0 - sin_h * 4.0);
                                    double wy_l = cy + (sin_h * -38.0 + cos_h * 4.0);
                                    double wx_r = cx + (cos_h * 38.0 - sin_h * 4.0);
                                    double wy_r = cy + (sin_h * 38.0 + cos_h * 4.0);

                                    /* Nacelle coordinates: (-13, +22) and (+13, +22) */
                                    double ex_l = cx + (cos_h * -13.0 - sin_h * 22.0);
                                    double ey_l = cy + (sin_h * -13.0 + cos_h * 22.0);
                                    double ex_r = cx + (cos_h * 13.0 - sin_h * 22.0);
                                    double ey_r = cy + (sin_h * 13.0 + cos_h * 22.0);

                                    spawn_vapor_streamer(wx_l, wy_l, dx * 3.5, dy * 3.5, 7.0, 0.65, th_state.heading);
                                    spawn_vapor_streamer(wx_r, wy_r, dx * 3.5, dy * 3.5, 7.0, 0.65, th_state.heading);
                                    spawn_vapor_streamer(ex_l, ey_l, dx * 2.5, dy * 2.5, 9.5, 0.80, th_state.heading);
                                    spawn_vapor_streamer(ex_r, ey_r, dx * 2.5, dy * 2.5, 9.5, 0.80, th_state.heading);

                                    needs_wake = TRUE;
                                    pthread_mutex_unlock(&v_mutex);

                                    if (!is_animating) {
                                        g_idle_add(wake_animation, NULL);
                                    }
                                }
                                idle_streak = 0;
                                sleep_us = 8000;
                            } else if (prev_x != -1) {
                                /* 6. Sudden Stop Detection: Nose rises and slowly settles */
                                if (th_state.is_moving && last_speed > 3.0) {
                                    pthread_mutex_lock(&v_mutex);
                                    th_state.pitch_flare = 0.40; /* Nose flares up on deceleration */
                                    th_state.is_moving = 0;
                                    th_state.scale = 0.95;
                                    needs_wake = TRUE;
                                    pthread_mutex_unlock(&v_mutex);
                                    if (!is_animating) g_idle_add(wake_animation, NULL);
                                }

                                idle_streak++;
                                if (idle_streak > 30) {
                                    audio_target_volume = 0.0f; /* Silence when stationary > 1s */
                                    sleep_us = 35000; /* ~28 Hz idle backoff */
                                } else if (idle_streak > 6) {
                                    audio_target_volume = 0.04f; /* Soft idle hum */
                                    sleep_us = 16000;
                                } else {
                                    sleep_us = 8000;
                                }
                            }
                            prev_x = cx;
                            prev_y = cy;
                        }
                    }
                }
            }
            close(fd);
        }
        usleep(sleep_us);
    }
    return NULL;
}

static void rebuild_overlays(GtkApplication *app) {
    if (!app) return;

    while (overlay_list != NULL) {
        MonitorOverlay *ov = (MonitorOverlay*)overlay_list->data;
        if (ov->window) gtk_window_destroy(GTK_WINDOW(ov->window));
        if (ov->monitor) g_object_unref(ov->monitor);
        g_free(ov);
        overlay_list = g_list_delete_link(overlay_list, overlay_list);
    }

    GdkDisplay *display = gdk_display_get_default();
    if (!display) return;

    GListModel *monitors = gdk_display_get_monitors(display);
    guint n = g_list_model_get_n_items(monitors);

    for (guint i = 0; i < n; i++) {
        GdkMonitor *mon = GDK_MONITOR(g_list_model_get_item(monitors, i));
        GdkRectangle geom;
        gdk_monitor_get_geometry(mon, &geom);

        MonitorOverlay *ov = g_new0(MonitorOverlay, 1);
        ov->monitor = g_object_ref(mon);
        ov->mon_x = geom.x;
        ov->mon_y = geom.y;
        ov->mon_w = geom.width;
        ov->mon_h = geom.height;

        ov->window = gtk_application_window_new(app);
        gtk_layer_init_for_window(GTK_WINDOW(ov->window));
        gtk_layer_set_monitor(GTK_WINDOW(ov->window), mon);
        gtk_layer_set_layer(GTK_WINDOW(ov->window), GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_namespace(GTK_WINDOW(ov->window), "gladius-smoke-trail");
        gtk_layer_set_keyboard_mode(GTK_WINDOW(ov->window), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        gtk_layer_set_anchor(GTK_WINDOW(ov->window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(ov->window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(ov->window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(ov->window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
        gtk_layer_set_exclusive_zone(GTK_WINDOW(ov->window), -1);

        gtk_widget_set_can_target(ov->window, FALSE);
        gtk_widget_set_can_focus(ov->window, FALSE);

        ov->drawing_area = gtk_drawing_area_new();
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ov->drawing_area), draw_flight_func, ov, NULL);
        gtk_widget_set_can_target(ov->drawing_area, FALSE);
        gtk_widget_set_can_focus(ov->drawing_area, FALSE);

        gtk_window_set_child(GTK_WINDOW(ov->window), ov->drawing_area);
        g_signal_connect(ov->window, "map", G_CALLBACK(on_map), NULL);

        gtk_window_present(GTK_WINDOW(ov->window));
        overlay_list = g_list_append(overlay_list, ov);
    }
}

static void on_monitors_changed(GListModel *list, guint position, guint removed, guint added, gpointer user_data) {
    (void)list; (void)position; (void)removed; (void)added;
    rebuild_overlays((GtkApplication*)user_data);
}

static void on_app_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    global_app = app;
    init_vapor_stamp();
    rebuild_overlays(app);

    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        GListModel *monitors = gdk_display_get_monitors(display);
        g_signal_connect(monitors, "items-changed", G_CALLBACK(on_monitors_changed), app);
    }

    /* Start Background Worker Threads */
    pthread_t poller_th, audio_th, mouse_btn_th;
    pthread_create(&poller_th, NULL, poll_thread_func, NULL);
    pthread_detach(poller_th);

    pthread_create(&audio_th, NULL, audio_thread_func, NULL);
    pthread_detach(audio_th);

    pthread_create(&mouse_btn_th, NULL, mouse_button_thread_func, NULL);
    pthread_detach(mouse_btn_th);
}

static void handle_exit_signal(int sig) {
    (void)sig;
    is_running = FALSE;
    /* Restore standard cursor if invisible was set */
    system("hyprctl eval 'hl.config({ cursor = { invisible = false } })' &>/dev/null || true");
    if (global_app) {
        g_idle_add(quit_app_idle, NULL);
    }
}

int main(int argc, char **argv) {
    /* Enforce single daemon instance */
    char lock_path[256];
    snprintf(lock_path, sizeof(lock_path), "/tmp/gladius_thunderhawk_%u.lock", getuid());
    int lock_fd = open(lock_path, O_CREAT | O_RDWR, 0600);
    if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        return 0; /* Already running */
    }

    signal(SIGINT, handle_exit_signal);
    signal(SIGTERM, handle_exit_signal);
    signal(SIGHUP, handle_exit_signal);

    GtkApplication *app = gtk_application_new("org.omarchy.gladius.thunderhawk", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    if (vapor_stamp) {
        cairo_surface_destroy(vapor_stamp);
    }
    close(lock_fd);
    unlink(lock_path);
    return status;
}
