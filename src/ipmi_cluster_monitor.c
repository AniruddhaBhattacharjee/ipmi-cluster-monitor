#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

#include <ipmi_monitoring.h>

#define LOG_DIR "sensor_data_logs"
#define LOG_BUFFER_SIZE (1024 * 1024)

typedef struct {
    int record_id;
    int sensor_number;
    int sensor_state;
    int sensor_units;
    int reading_type;
    char *name;
    double reading;
} spec_list;

typedef struct {
    char *hostname;
    char *username;
    char *password;
    int logging_enabled;
    double sample_interval;

    ipmi_monitoring_ctx_t ctx;
    struct ipmi_monitoring_ipmi_config config;

    spec_list *sensors;
    int sensor_count;
    unsigned int *record_ids;

    pthread_t sampler_thread;
    pthread_t logger_thread;
    int sampler_started;
    int logger_started;

    pthread_mutex_t sensor_mutex;
    pthread_cond_t log_cond;
    unsigned long sample_generation;
    struct timespec sample_timestamp;

    FILE *log_fp;
    char *log_buffer;
    char log_path[PATH_MAX];
} bmc_runtime;

static volatile sig_atomic_t g_stop = 0;

static void signal_handler(int sig) {
    (void)sig;
    g_stop = 1;
}

static const char *unit_string(int unit) {
    switch (unit) {
        case IPMI_MONITORING_SENSOR_UNITS_CELSIUS: return "C";
        case IPMI_MONITORING_SENSOR_UNITS_FAHRENHEIT: return "F";
        case IPMI_MONITORING_SENSOR_UNITS_VOLTS: return "V";
        case IPMI_MONITORING_SENSOR_UNITS_AMPS: return "A";
        case IPMI_MONITORING_SENSOR_UNITS_WATTS: return "W";
        case IPMI_MONITORING_SENSOR_UNITS_RPM: return "RPM";
        case IPMI_MONITORING_SENSOR_UNITS_PERCENT: return "%";
        default: return "";
    }
}

static char *trim(char *s) {
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (*s == '\0') return s;
    end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) *end-- = '\0';
    return s;
}

static int is_log_token(const char *s) {
    return s && (!strcasecmp(s, "log") || !strcasecmp(s, "logging") || !strcasecmp(s, "yes") || !strcmp(s, "1"));
}

static void sanitize_filename(const char *src, char *dst, size_t dst_size) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dst_size; i++) {
        char c = src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') dst[j++] = c;
        else dst[j++] = '_';
    }
    dst[j] = '\0';
}

static void sleep_seconds(double seconds) {
    struct timespec req, rem;
    if (seconds <= 0.0) return;
    req.tv_sec = (time_t)seconds;
    req.tv_nsec = (long)((seconds - (double)req.tv_sec) * 1000000000.0);
    while (!g_stop && nanosleep(&req, &rem) < 0 && errno == EINTR) req = rem;
}

static void free_sensor_list(spec_list *list, int count) {
    if (!list) return;
    for (int i = 0; i < count; i++) free(list[i].name);
    free(list);
}

static spec_list *get_specific_sensors(ipmi_monitoring_ctx_t ctx, const char *hostname, struct ipmi_monitoring_ipmi_config *config,
                                       const enum ipmi_monitoring_sensor_units *target_units, int num_units, int *out_count) {
    unsigned int flags = IPMI_MONITORING_SENSOR_READING_FLAGS_DISCRETE_READING;
    int sensor_count = ipmi_monitoring_sensor_readings_by_record_id(ctx, hostname, config, flags, NULL, 0, NULL, NULL);
    if (out_count) *out_count = 0;
    if (sensor_count < 0) {
        fprintf(stderr, "[%s] Sensor discovery failed: %s\n", hostname, ipmi_monitoring_ctx_errormsg(ctx));
        return NULL;
    }

    spec_list *list = NULL;
    int selected_count = 0;

    for (int i = 0; i < sensor_count; i++) {
        int record_id = ipmi_monitoring_sensor_read_record_id(ctx);
        int sensor_number = ipmi_monitoring_sensor_read_sensor_number(ctx);
        int sensor_state = ipmi_monitoring_sensor_read_sensor_state(ctx);
        int sensor_units = ipmi_monitoring_sensor_read_sensor_units(ctx);
        int reading_type = ipmi_monitoring_sensor_read_sensor_reading_type(ctx);
        char *sensor_name = ipmi_monitoring_sensor_read_sensor_name(ctx);
        void *reading = ipmi_monitoring_sensor_read_sensor_reading(ctx);

        int unit_match = (target_units == NULL || num_units <= 0);
        if (!unit_match) {
            for (int j = 0; j < num_units; j++) {
                if (sensor_units == (int)target_units[j]) {
                    unit_match = 1;
                    break;
                }
            }
        }

        if (unit_match && reading && reading_type == IPMI_MONITORING_SENSOR_READING_TYPE_DOUBLE){
            spec_list *tmp = realloc(list, (size_t)(selected_count + 1) * sizeof(*list));
            if (!tmp) {
                fprintf(stderr, "[%s] Sensor-list allocation failed\n", hostname);
                free_sensor_list(list, selected_count);
                return NULL;
            }
            list = tmp;
            spec_list *s = &list[selected_count];
            s->record_id = record_id;
            s->sensor_number = sensor_number;
            s->sensor_state = sensor_state;
            s->sensor_units = sensor_units;
            s->reading_type = reading_type;
            s->name = strdup(sensor_name ? sensor_name : "");
            if (!s->name) {
                fprintf(stderr, "[%s] Sensor-name allocation failed\n", hostname);
                free_sensor_list(list, selected_count + 1);
                return NULL;
            }
            s->reading = *((double *)reading);
            selected_count++;
        }

        if (i < sensor_count - 1) ipmi_monitoring_sensor_iterator_next(ctx);
    }

    if (out_count) *out_count = selected_count;
    return list;
}

static int update_sensor_readings(bmc_runtime *bmc){
    unsigned int flags = IPMI_MONITORING_SENSOR_READING_FLAGS_DISCRETE_READING;
    if (!bmc->sensors || bmc->sensor_count <= 0) return 0;

    int returned = ipmi_monitoring_sensor_readings_by_record_id(
        bmc->ctx, bmc->hostname, &bmc->config, flags,
        bmc->record_ids, (unsigned int)bmc->sensor_count, NULL, NULL);

    if (returned < 0) {
        fprintf(stderr, "[%s] Sensor update failed: %s\n", bmc->hostname, ipmi_monitoring_ctx_errormsg(bmc->ctx));
        return -1;
    }

    pthread_mutex_lock(&bmc->sensor_mutex);
    for (int i = 0; i < returned; i++) {
        int record_id = ipmi_monitoring_sensor_read_record_id(bmc->ctx);
        int sensor_state = ipmi_monitoring_sensor_read_sensor_state(bmc->ctx);
        int reading_type = ipmi_monitoring_sensor_read_sensor_reading_type(bmc->ctx);
        void *reading = ipmi_monitoring_sensor_read_sensor_reading(bmc->ctx);

        if (reading && reading_type == IPMI_MONITORING_SENSOR_READING_TYPE_DOUBLE) {
            for (int j = 0; j < bmc->sensor_count; j++) {
                if (bmc->sensors[j].record_id == record_id) {
                    bmc->sensors[j].sensor_state = sensor_state;
                    bmc->sensors[j].reading = *((double *)reading);
                    break;
                }
            }
        }

        if (i < returned - 1) ipmi_monitoring_sensor_iterator_next(bmc->ctx);
    }

    clock_gettime(CLOCK_REALTIME, &bmc->sample_timestamp);
    bmc->sample_generation++;
    pthread_cond_signal(&bmc->log_cond);
    pthread_mutex_unlock(&bmc->sensor_mutex);
    return returned;
}

static void *sampler_thread_main(void *arg) {
    bmc_runtime *bmc = arg;
    while (!g_stop) {
        update_sensor_readings(bmc);
        sleep_seconds(bmc->sample_interval);
    }
    return NULL;
}

static void *logger_thread_main(void *arg) {
    bmc_runtime *bmc = arg;
    double *snapshot = malloc((size_t)bmc->sensor_count * sizeof(*snapshot));
    if (!snapshot) {
        fprintf(stderr, "[%s] Logger snapshot allocation failed!\n", bmc->hostname);
        return NULL;
    }

    unsigned long last_generation = 0;
    while (1) {
        struct timespec ts;
        unsigned long generation;

        pthread_mutex_lock(&bmc->sensor_mutex);
        while (!g_stop && bmc->sample_generation == last_generation)
            pthread_cond_wait(&bmc->log_cond, &bmc->sensor_mutex);

        if (g_stop && bmc->sample_generation == last_generation) {
            pthread_mutex_unlock(&bmc->sensor_mutex);
            break;
        }

        generation = bmc->sample_generation;
        ts = bmc->sample_timestamp;
        for (int i = 0; i < bmc->sensor_count; i++) snapshot[i] = bmc->sensors[i].reading;
        pthread_mutex_unlock(&bmc->sensor_mutex);

        // One row per sample: timestamp,name1:reading1:unit1,name2:reading2:unit2
        fprintf(bmc->log_fp, "%lld.%09ld", (long long)ts.tv_sec, ts.tv_nsec);
        for (int i = 0; i < bmc->sensor_count; i++) {
            fprintf(bmc->log_fp, ",%s:%.6f:%s", bmc->sensors[i].name, snapshot[i], unit_string(bmc->sensors[i].sensor_units));
        }
        fputc('\n', bmc->log_fp);
        last_generation = generation;
    }

    fflush(bmc->log_fp);
    free(snapshot);
    return NULL;
}

static int configure_bmc(bmc_runtime *bmc) {
    bmc->ctx = ipmi_monitoring_ctx_create();
    if (!bmc->ctx) {
        fprintf(stderr, "[%s] Could not create IPMI monitoring context\n", bmc->hostname);
        return -1;
    }
    if (ipmi_monitoring_ctx_sensor_config_file(bmc->ctx, NULL) < 0) {
        fprintf(stderr, "[%s] Sensor config error: %s\n", bmc->hostname, ipmi_monitoring_ctx_errormsg(bmc->ctx));
        return -1;
    }

    memset(&bmc->config, 0, sizeof(bmc->config));
    bmc->config.driver_type = -1;
    bmc->config.disable_auto_probe = 0;
    bmc->config.driver_address = 0;
    bmc->config.register_spacing = 0;
    bmc->config.driver_device = NULL;
    bmc->config.protocol_version = IPMI_MONITORING_PROTOCOL_VERSION_2_0;
    bmc->config.username = bmc->username;
    bmc->config.password = bmc->password;
    bmc->config.k_g = NULL;
    bmc->config.k_g_len = 0;
    bmc->config.privilege_level = IPMI_MONITORING_PRIVILEGE_LEVEL_ADMIN;
    bmc->config.authentication_type = -1;
    bmc->config.cipher_suite_id = 3;
    bmc->config.session_timeout_len = 0;
    bmc->config.retransmission_timeout_len = 0;
    bmc->config.workaround_flags = 0;

    enum ipmi_monitoring_sensor_units target_units[] = {
        IPMI_MONITORING_SENSOR_UNITS_CELSIUS,
        //IPMI_MONITORING_SENSOR_UNITS_FAHRENHEIT,
        IPMI_MONITORING_SENSOR_UNITS_VOLTS,
        IPMI_MONITORING_SENSOR_UNITS_AMPS,
        IPMI_MONITORING_SENSOR_UNITS_WATTS,
        //IPMI_MONITORING_SENSOR_UNITS_PERCENT,
        //IPMI_MONITORING_SENSOR_UNITS_RPM,
    };
    //IPMI_MONITORING_SENSOR_UNITS_RPM,
    //IPMI_MONITORING_SENSOR_UNITS_PERCENT
    bmc->sensors = get_specific_sensors(
        bmc->ctx, bmc->hostname, &bmc->config,
        target_units, (int)(sizeof(target_units) / sizeof(target_units[0])),
        &bmc->sensor_count);

    if (!bmc->sensors || bmc->sensor_count <= 0) {
        fprintf(stderr, "[%s] No selected sensors discovered\n", bmc->hostname);
        return -1;
    }

    bmc->record_ids = malloc((size_t)bmc->sensor_count * sizeof(*bmc->record_ids));
    if (!bmc->record_ids) return -1;
    for (int i = 0; i < bmc->sensor_count; i++) bmc->record_ids[i] = (unsigned int)bmc->sensors[i].record_id;

    clock_gettime(CLOCK_REALTIME, &bmc->sample_timestamp);
    bmc->sample_generation = 1;
    return 0;
}

static int clear_bmc_log(bmc_runtime *bmc){
    char safe_host[256];
    sanitize_filename(bmc->hostname, safe_host, sizeof(safe_host));
    snprintf(bmc->log_path, sizeof(bmc->log_path), "%s/%s.csv", LOG_DIR, safe_host);
    bmc->log_fp = fopen(bmc->log_path, "w");
    if (!bmc->log_fp){
        fprintf(stderr, "[%s] Cannot open log %s: %s\n", bmc->hostname, bmc->log_path, strerror(errno));
        return -1;
    }
    fclose(bmc->log_fp);
    return 0;
}

static int open_bmc_log(bmc_runtime *bmc) {
    char safe_host[256];
    sanitize_filename(bmc->hostname, safe_host, sizeof(safe_host));
    snprintf(bmc->log_path, sizeof(bmc->log_path), "%s/%s.csv", LOG_DIR, safe_host);

    bmc->log_fp = fopen(bmc->log_path, "a");
    if (!bmc->log_fp) {
        fprintf(stderr, "[%s] Cannot open log %s: %s\n", bmc->hostname, bmc->log_path, strerror(errno));
        return -1;
    }

    /* Large stdio buffer reduces write()/filesystem overhead during frequent logging. */
    bmc->log_buffer = malloc(LOG_BUFFER_SIZE);
    if (bmc->log_buffer) setvbuf(bmc->log_fp, bmc->log_buffer, _IOFBF, LOG_BUFFER_SIZE);
    return 0;
}

static int append_bmc(bmc_runtime **bmcs, int *count, int *capacity, const char *host, const char *user, const char *pass, int logging_enabled, double interval) {
    if (*count == *capacity) {
        int new_capacity = (*capacity == 0) ? 4 : (*capacity * 2);
        bmc_runtime *tmp = realloc(*bmcs, (size_t)new_capacity * sizeof(**bmcs));
        if (!tmp) return -1;
        *bmcs = tmp;
        *capacity = new_capacity;
    }

    bmc_runtime *b = &(*bmcs)[*count];
    memset(b, 0, sizeof(*b));
    b->hostname = strdup(host);
    b->username = strdup(user);
    b->password = strdup(pass);
    b->logging_enabled = logging_enabled;
    b->sample_interval = interval;
    if (!b->hostname || !b->username || !b->password) return -1;
    if (pthread_mutex_init(&b->sensor_mutex, NULL) != 0) return -1;
    if (pthread_cond_init(&b->log_cond, NULL) != 0) return -1;
    (*count)++;
    return 0;
}

static int load_bmc_file(const char *path, bmc_runtime **out_bmcs, int *out_count, double interval) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "Cannot open BMC file %s: %s\n", path, strerror(errno));
        return -1;
    }

    bmc_runtime *bmcs = NULL;
    int count = 0, capacity = 0;
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t nread;
    int line_no = 0;

    while ((nread = getline(&line, &line_cap, fp)) >= 0) {
        (void)nread;
        line_no++;
        char *p = trim(line);
        if (*p == '\0' || *p == '#') continue;

        char *save = NULL;
        char *host = strtok_r(p, ",", &save);
        char *user = strtok_r(NULL, ",", &save);
        char *pass = strtok_r(NULL, ",", &save);
        char *opt = strtok_r(NULL, ",", &save);

        if (!host || !user || !pass) {
            fprintf(stderr, "Ignoring malformed line %d in %s\n", line_no, path);
            continue;
        }

        host = trim(host); user = trim(user); pass = trim(pass); if (opt) opt = trim(opt);
        if (append_bmc(&bmcs, &count, &capacity, host, user, pass, is_log_token(opt), interval) < 0) {
            fprintf(stderr, "Failed to store BMC entry from line %d\n", line_no);
            free(line);
            fclose(fp);
            return -1;
        }
    }

    free(line);
    fclose(fp);
    *out_bmcs = bmcs;
    *out_count = count;
    return count > 0 ? 0 : -1;
}

static void cleanup_bmc(bmc_runtime *bmc) {
    if (bmc->log_fp) fclose(bmc->log_fp);
    free(bmc->log_buffer);
    free(bmc->record_ids);
    free_sensor_list(bmc->sensors, bmc->sensor_count);
    if (bmc->ctx) ipmi_monitoring_ctx_destroy(bmc->ctx);
    pthread_cond_destroy(&bmc->log_cond);
    pthread_mutex_destroy(&bmc->sensor_mutex);
    free(bmc->hostname);
    free(bmc->username);
    if (bmc->password) {
        memset(bmc->password, 0, strlen(bmc->password));
        free(bmc->password);
    }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <BMC_FILE> <INTERVAL_SECONDS>\n", argv[0]);
        fprintf(stderr, "BMC file format: IP,USERNAME,PASSWORD[,log]\n");
        return EXIT_FAILURE;
    }

    char *end = NULL;
    double interval = strtod(argv[2], &end);
    if (!end || *end != '\0' || interval <= 0.0) {
        fprintf(stderr, "Sampling interval must be a number > 0 seconds\n");
        return EXIT_FAILURE;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int errnum;
    if (ipmi_monitoring_init(0, &errnum) < 0) {
        fprintf(stderr, "ipmi_monitoring_init failed: %s\n", ipmi_monitoring_ctx_strerror(errnum));
        return EXIT_FAILURE;
    }

    bmc_runtime *bmcs = NULL;
    int bmc_count = 0;
    if (load_bmc_file(argv[1], &bmcs, &bmc_count, interval) < 0) {
        fprintf(stderr, "No valid BMC entries loaded\n");
        return EXIT_FAILURE;
    }

    if (mkdir(LOG_DIR, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "Cannot create %s: %s\n", LOG_DIR, strerror(errno));
        return EXIT_FAILURE;
    }

    int active = 0;
    for (int i = 0; i < bmc_count; i++) {
        bmc_runtime *b = &bmcs[i];
        printf("[%s] Initializing%s...\n", b->hostname, b->logging_enabled ? " (logging enabled)" : "");
        if (configure_bmc(b) < 0) {
            fprintf(stderr, "[%s] Initialization failed; skipping\n", b->hostname);
            continue;
        }

        printf("[%s] Selected %d sensors\n", b->hostname, b->sensor_count);

        if (b->logging_enabled) {
            if (clear_bmc_log(b) == 0){
                printf("[%s] Cleared log file <%s>\n", b->hostname, b->log_path);
            } else{
                fprintf(stderr, "[%s-!] Failed to Clear log file <%s>!\n", b->hostname, b->log_path);
            }
            if (open_bmc_log(b) == 0 && pthread_create(&b->logger_thread, NULL, logger_thread_main, b) == 0) {
                b->logger_started = 1;
                printf("[%s] Logging to %s\n", b->hostname, b->log_path);
            } else {
                fprintf(stderr, "[%s-!] Logger startup failed\n", b->hostname);
            }
        }

        if (pthread_create(&b->sampler_thread, NULL, sampler_thread_main, b) == 0) {
            b->sampler_started = 1;
            active++;
        } else {
            fprintf(stderr, "[%s] Sampler thread startup failed\n", b->hostname);
        }
    }

    if (active == 0) {
        fprintf(stderr, "No BMC sampler threads started\n");
        g_stop = 1;
    }

    while (!g_stop) sleep(1);
    printf("\n[*] SIGINT Detected, stopping acquisition!\n");
    for (int i = 0; i < bmc_count; i++) {
        pthread_mutex_lock(&bmcs[i].sensor_mutex);
        pthread_cond_broadcast(&bmcs[i].log_cond);
        pthread_mutex_unlock(&bmcs[i].sensor_mutex);
    }

    for (int i = 0; i < bmc_count; i++) {
        if (bmcs[i].sampler_started) pthread_join(bmcs[i].sampler_thread, NULL);
        if (bmcs[i].logger_started) pthread_join(bmcs[i].logger_thread, NULL);
    }

    for (int i = 0; i < bmc_count; i++) cleanup_bmc(&bmcs[i]);
    free(bmcs);
    printf("[*] Telemetry Complete.\n");
    return EXIT_SUCCESS;
}
