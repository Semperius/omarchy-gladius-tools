#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <pthread.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <cairo.h>

#define MAX_PARTICLES 400
#define STAMP_SIZE 128

typedef struct {
    double x, y;
    double vx, vy;
    double r0, r_max;
    double age, lifetime;
    double alpha0;
    double r_col, g_col, b_col;
    gboolean active;
} SmokeParticle;

typedef struct {
    double x, y;
    double radius;
    double alpha;
    double r_col, g_col, b_col;
} RenderParticle;

typedef struct {
    GtkWidget *window;
    GtkWidget *drawing_area;
    GdkMonitor *monitor;
    int mon_x, mon_y, mon_w, mon_h;
} MonitorOverlay;

static SmokeParticle particles[MAX_PARTICLES];
static int particle_head = 0;
static pthread_mutex_t p_mutex = PTHREAD_MUTEX_INITIALIZER;

// Double-buffered render snapshot (decoupled from poller mutex)
static RenderParticle render_snapshot[MAX_PARTICLES];
static int render_snapshot_count = 0;
static pthread_mutex_t render_mutex = PTHREAD_MUTEX_INITIALIZER;

static volatile gboolean is_running = TRUE;
static volatile gboolean is_animating = FALSE;
static volatile gboolean needs_wake = FALSE;

static GList *overlay_list = NULL;
static GtkApplication *global_app = NULL;
static guint tick_source_id = 0;
static char lock_file_path[512] = "";

// Pre-cached high-performance smoke stamp
static cairo_surface_t *smoke_stamp = NULL;

static void init_smoke_stamp(void) {
    smoke_stamp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, STAMP_SIZE, STAMP_SIZE);
    cairo_t *cr = cairo_create(smoke_stamp);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    double mid = STAMP_SIZE / 2.0;
    double rad = mid;

    // Volumetric 3D sphere gradient with offset specular illumination
    cairo_pattern_t *pat = cairo_pattern_create_radial(
        mid - rad * 0.15, mid - rad * 0.15, rad * 0.05,
        mid, mid, rad
    );
    cairo_pattern_add_color_stop_rgba(pat, 0.0, 1.0, 1.0, 1.0, 1.0);
    cairo_pattern_add_color_stop_rgba(pat, 0.38, 0.95, 0.96, 1.0, 0.72);
    cairo_pattern_add_color_stop_rgba(pat, 0.72, 0.85, 0.88, 0.95, 0.25);
    cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.75, 0.78, 0.85, 0.0);

    cairo_set_source(cr, pat);
    cairo_arc(cr, mid, mid, rad, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);
    cairo_destroy(cr);
}

static double rand_range(double min, double max) {
    return min + (double)rand() / (double)RAND_MAX * (max - min);
}

static void spawn_particle(double x, double y, double move_vx, double move_vy) {
    SmokeParticle *p = &particles[particle_head];
    particle_head = (particle_head + 1) % MAX_PARTICLES;

    double angle = rand_range(0, 2 * M_PI);
    double dist = rand_range(0, 7);
    p->x = x + cos(angle) * dist;
    p->y = y + sin(angle) * dist;

    // Opposite drift, thermal buoyancy, and slight swirl
    p->vx = -move_vx * 0.12 + rand_range(-15, 15);
    p->vy = -move_vy * 0.12 - rand_range(12, 32);

    p->r0 = rand_range(16, 22);
    p->r_max = rand_range(48, 72);
    p->age = 0.0;
    p->lifetime = rand_range(0.55, 0.85);
    p->alpha0 = rand_range(0.40, 0.55);

    double base = rand_range(0.82, 0.95);
    p->r_col = base * 0.94;
    p->g_col = base * 0.97;
    p->b_col = base * 1.04;
    if (p->b_col > 1.0) p->b_col = 1.0;

    p->active = TRUE;
}

static void spawn_wake_burst(double x, double y) {
    for (int i = 0; i < 6; i++) {
        SmokeParticle *p = &particles[particle_head];
        particle_head = (particle_head + 1) % MAX_PARTICLES;

        double angle = (2.0 * M_PI / 6.0) * i + rand_range(-0.15, 0.15);
        double speed = rand_range(25.0, 50.0);
        p->x = x;
        p->y = y;
        p->vx = cos(angle) * speed;
        p->vy = sin(angle) * speed;
        p->r0 = rand_range(16, 22);
        p->r_max = rand_range(48, 68);
        p->age = 0.0;
        p->lifetime = rand_range(0.40, 0.65);
        p->alpha0 = 0.70;

        p->r_col = 0.96;
        p->g_col = 0.98;
        p->b_col = 1.0;
        p->active = TRUE;
    }
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

static void draw_smoke_func(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    (void)area;
    MonitorOverlay *ov = (MonitorOverlay*)user_data;

    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // Read from thread-local snapshot: ZERO lock contention on poller mutex
    RenderParticle local_copy[MAX_PARTICLES];
    int count = 0;

    pthread_mutex_lock(&render_mutex);
    count = render_snapshot_count;
    if (count > 0) {
        memcpy(local_copy, render_snapshot, count * sizeof(RenderParticle));
    }
    pthread_mutex_unlock(&render_mutex);

    if (count == 0 || !smoke_stamp) return;

    if (ov->monitor) {
        GdkRectangle geom;
        gdk_monitor_get_geometry(ov->monitor, &geom);
        ov->mon_x = geom.x;
        ov->mon_y = geom.y;
        ov->mon_w = geom.width;
        ov->mon_h = geom.height;
    }

    double half_stamp = STAMP_SIZE / 2.0;

    for (int i = 0; i < count; i++) {
        RenderParticle *p = &local_copy[i];

        double lx = p->x - ov->mon_x;
        double ly = p->y - ov->mon_y;
        double r = p->radius;

        if (lx + r < 0 || lx - r > width || ly + r < 0 || ly - r > height) {
            continue;
        }

        double scale = r / half_stamp;

        cairo_save(cr);
        cairo_translate(cr, lx, ly);
        cairo_scale(cr, scale, scale);
        cairo_translate(cr, -half_stamp, -half_stamp);

        cairo_set_source_surface(cr, smoke_stamp, 0, 0);
        cairo_paint_with_alpha(cr, p->alpha);
        cairo_restore(cr);
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
            tick_source_id = g_timeout_add(16, on_frame_tick, NULL);
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

    // Fast snapshot update
    RenderParticle next_snap[MAX_PARTICLES];
    int snap_count = 0;

    pthread_mutex_lock(&p_mutex);
    int active = 0;
    for (int i = 0; i < MAX_PARTICLES; i++) {
        SmokeParticle *p = &particles[i];
        if (!p->active) continue;

        p->age += dt;
        if (p->age >= p->lifetime) {
            p->active = FALSE;
            continue;
        }

        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->vx *= (1.0 - 0.35 * dt);
        p->vy *= (1.0 - 0.18 * dt);

        double progress = p->age / p->lifetime;
        if (progress < 1.0 && snap_count < MAX_PARTICLES) {
            RenderParticle *rp = &next_snap[snap_count++];
            rp->x = p->x;
            rp->y = p->y;
            rp->radius = p->r0 + (p->r_max - p->r0) * pow(progress, 0.65);
            rp->alpha = p->alpha0 * pow(1.0 - progress, 1.4);
            rp->r_col = p->r_col;
            rp->g_col = p->g_col;
            rp->b_col = p->b_col;
        }
        active++;
    }

    gboolean stop_animating = (active == 0 && !needs_wake);
    needs_wake = FALSE;
    pthread_mutex_unlock(&p_mutex);

    // Atomic snapshot publish
    pthread_mutex_lock(&render_mutex);
    render_snapshot_count = snap_count;
    if (snap_count > 0) {
        memcpy(render_snapshot, next_snap, snap_count * sizeof(RenderParticle));
    }
    pthread_mutex_unlock(&render_mutex);

    if (stop_animating) {
        is_animating = FALSE;
        tick_source_id = 0;
        for (GList *l = overlay_list; l != NULL; l = l->next) {
            MonitorOverlay *ov = (MonitorOverlay*)l->data;
            gtk_widget_queue_draw(ov->drawing_area);
        }
        return G_SOURCE_REMOVE;
    }

    // Queue redraw
    for (GList *l = overlay_list; l != NULL; l = l->next) {
        MonitorOverlay *ov = (MonitorOverlay*)l->data;
        gtk_widget_queue_draw(ov->drawing_area);
    }

    return G_SOURCE_CONTINUE;
}

static gboolean quit_app_idle(gpointer data) {
    (void)data;
    if (global_app) {
        g_application_quit(G_APPLICATION(global_app));
    }
    return G_SOURCE_REMOVE;
}

// Background poller thread with socket timeout and failure threshold
static void *poll_thread_func(void *arg) {
    (void)arg;
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!runtime || !sig) {
        fprintf(stderr, "gladius-smoke-trail: Missing Hyprland session variables\n");
        return NULL;
    }

    char sock_path[512];
    snprintf(sock_path, sizeof(sock_path), "%s/hypr/%s/.socket.sock", runtime, sig);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "gladius-smoke-trail: Socket path exceeds buffer length\n");
        return NULL;
    }
    memcpy(addr.sun_path, sock_path, strlen(sock_path) + 1);

    int prev_x = -1, prev_y = -1;
    int consecutive_errors = 0;
    const int max_errors = 150; // ~1.2s without compositor -> graceful exit
    int idle_streak = 0;
    useconds_t sleep_us = 8000;

    // 50ms socket read/write timeout
    struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };

    while (is_running) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) {
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                consecutive_errors = 0;
                const char *cmd = "cursorpos";
                ssize_t w_n = write(fd, cmd, strlen(cmd));
                if (w_n > 0) {
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

                                if (dist > 2.0) {
                                    gboolean was_idle = (idle_streak >= 50); // ~1.5s+ of stationary cursor
                                    pthread_mutex_lock(&p_mutex);
                                    if (was_idle) {
                                        spawn_wake_burst(cx, cy);
                                    }

                                    int steps = (int)(dist / 14.0);
                                    if (steps < 1) steps = 1;
                                    if (steps > 5) steps = 5;

                                    for (int s = 0; s < steps; s++) {
                                        double f = (double)(s + 1) / (double)steps;
                                        double px = prev_x + dx * f;
                                        double py = prev_y + dy * f;
                                        spawn_particle(px, py, dx * 6.0, dy * 6.0);
                                    }
                                    needs_wake = TRUE;
                                    pthread_mutex_unlock(&p_mutex);

                                    if (!is_animating) {
                                        g_idle_add(wake_animation, NULL);
                                    }
                                }
                                idle_streak = 0;
                                sleep_us = 8000;
                            } else if (prev_x != -1) {
                                idle_streak++;
                                if (idle_streak > 30) {
                                    sleep_us = 35000; // ~28 Hz idle backoff
                                } else if (idle_streak > 8) {
                                    sleep_us = 16000; // ~60 Hz transition
                                } else {
                                    sleep_us = 8000;
                                }
                            }
                            prev_x = cx;
                            prev_y = cy;
                        }
                    }
                }
            } else {
                consecutive_errors++;
                if (consecutive_errors >= max_errors) {
                    // Compositor connection lost; shut down daemon gracefully
                    is_running = FALSE;
                    g_idle_add(quit_app_idle, NULL);
                    close(fd);
                    break;
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
        if (ov->window) {
            gtk_window_destroy(GTK_WINDOW(ov->window));
        }
        if (ov->monitor) {
            g_object_unref(ov->monitor);
        }
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
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ov->drawing_area), draw_smoke_func, ov, NULL);
        gtk_widget_set_can_target(ov->drawing_area, FALSE);
        gtk_widget_set_can_focus(ov->drawing_area, FALSE);

        gtk_window_set_child(GTK_WINDOW(ov->window), ov->drawing_area);
        g_signal_connect(ov->window, "map", G_CALLBACK(on_map), NULL);

        gtk_window_present(GTK_WINDOW(ov->window));
        overlay_list = g_list_append(overlay_list, ov);
        g_object_unref(mon);
    }
}

static void on_monitors_changed(GListModel *model, guint position, guint removed, guint added, gpointer user_data) {
    (void)model;
    (void)position;
    (void)removed;
    (void)added;
    rebuild_overlays((GtkApplication*)user_data);
}

static void activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    global_app = app;
    init_smoke_stamp();

    GdkDisplay *display = gdk_display_get_default();

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, "window, drawingarea { background-color: transparent; }");
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    GListModel *monitors = gdk_display_get_monitors(display);
    g_signal_connect(monitors, "items-changed", G_CALLBACK(on_monitors_changed), app);

    rebuild_overlays(app);

    pthread_t tid;
    pthread_create(&tid, NULL, poll_thread_func, NULL);
    pthread_detach(tid);
}

static void handle_signal(int sig) {
    (void)sig;
    is_running = FALSE;
    if (lock_file_path[0]) {
        unlink(lock_file_path);
    }
    if (global_app) {
        g_application_quit(G_APPLICATION(global_app));
    }
}

int main(int argc, char **argv) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    snprintf(lock_file_path, sizeof(lock_file_path), "%s/gladius-smoke-trail.lock", runtime ? runtime : "/tmp");

    int lock_fd = open(lock_file_path, O_CREAT | O_RDWR, 0600);
    if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        if (argc > 1 && (strcmp(argv[1], "--kill") == 0 || strcmp(argv[1], "-k") == 0)) {
            char pid_buf[32];
            int read_n = read(lock_fd, pid_buf, sizeof(pid_buf) - 1);
            if (read_n > 0) {
                pid_buf[read_n] = '\0';
                pid_t running_pid = atoi(pid_buf);
                if (running_pid > 0) kill(running_pid, SIGTERM);
            }
            return 0;
        }
        printf("gladius-smoke-trail is already running.\n");
        return 0;
    }

    if (lock_fd >= 0) {
        ftruncate(lock_fd, 0);
        dprintf(lock_fd, "%d\n", getpid());
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    srand(time(NULL));
    GtkApplication *app = gtk_application_new("org.omarchy.gladius.smoketrail", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);

    if (smoke_stamp) {
        cairo_surface_destroy(smoke_stamp);
    }
    g_object_unref(app);

    if (lock_file_path[0]) {
        unlink(lock_file_path);
    }
    return status;
}
