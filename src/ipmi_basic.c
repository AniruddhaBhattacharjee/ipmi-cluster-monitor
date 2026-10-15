#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <ipmi_monitoring.h>

static const char *state_string(int state)
{
    switch (state) {
        case IPMI_MONITORING_STATE_NOMINAL:  return "Nominal";
        case IPMI_MONITORING_STATE_WARNING:  return "Warning";
        case IPMI_MONITORING_STATE_CRITICAL: return "Critical";
        default:                             return "Unknown";
    }
}

static const char *unit_string(int unit)
{
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
typedef struct power_sensors{
    int record_id;
    int sensor_number;
    int sensor_state;
    int sensor_units;
    int reading_type;
    char *name;
    double reading;
}spec_list;
int power_sensor_count = 0;

static void print_timestamp(void) {
    time_t now = time(NULL);
    struct tm tm_now;
    char buf[64];

    localtime_r(&now, &tm_now);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_now);

    printf("%s", buf);
}

static int sample_sensors(ipmi_monitoring_ctx_t ctx, const char *hostname, struct ipmi_monitoring_ipmi_config *config, int first_sample) {
    unsigned int flags = 0;
    int sensor_count;
    (void) first_sample;
    /*
     * Force SDR re-read only on the first sample.
     * Subsequent samples can reuse FreeIPMI's SDR cache.
     */
    if (first_sample)
        flags |= IPMI_MONITORING_SENSOR_READING_FLAGS_REREAD_SDR_CACHE;

    /*
     * Include discrete sensors as well as analog sensors.
     */
    flags |= IPMI_MONITORING_SENSOR_READING_FLAGS_DISCRETE_READING;

    sensor_count = ipmi_monitoring_sensor_readings_by_record_id( ctx, hostname, config, flags, NULL, 0, NULL, NULL);

    if (sensor_count < 0) {
        fprintf(stderr, "[F] Sensor query failed: %s\n", ipmi_monitoring_ctx_errormsg(ctx));
        return -1;
    }

    printf("\n[F] Found %d sensors\n", sensor_count);
    printf("[F] Timestamp,RecordID,SensorNumber,SensorName,State,Reading,Units\n");

    for (int i = 0; i < sensor_count; i++) {
        int record_id;
        int sensor_number;
        int sensor_state;
        int sensor_units;
        int reading_type;

        char *sensor_name;
        void *reading;

        record_id = ipmi_monitoring_sensor_read_record_id(ctx);
        sensor_number = ipmi_monitoring_sensor_read_sensor_number(ctx);
        sensor_name = ipmi_monitoring_sensor_read_sensor_name(ctx);
        sensor_state = ipmi_monitoring_sensor_read_sensor_state(ctx);
        sensor_units = ipmi_monitoring_sensor_read_sensor_units(ctx);
        reading_type = ipmi_monitoring_sensor_read_sensor_reading_type(ctx);
        reading = ipmi_monitoring_sensor_read_sensor_reading(ctx);

        print_timestamp();

        printf(",%d,%d,\"%s\",%s,", record_id, sensor_number, sensor_name ? sensor_name : "", state_string(sensor_state));

        if (reading == NULL) {
            printf("N/A");
        } else if (reading_type == IPMI_MONITORING_SENSOR_READING_TYPE_DOUBLE) {
            printf("%.6f", *((double *)reading));
        } else if (reading_type == IPMI_MONITORING_SENSOR_READING_TYPE_UNSIGNED_INTEGER32) {
            printf("%u", *((uint32_t *)reading));
        } else if (reading_type == IPMI_MONITORING_SENSOR_READING_TYPE_UNSIGNED_INTEGER8_BOOL) {
            printf("%u", *((uint8_t *)reading));
        } else {
            printf("N/A");
        }

        printf(",%s\n", unit_string(sensor_units));

        ipmi_monitoring_sensor_iterator_next(ctx);
    }

    return sensor_count;
}

spec_list* get_power_sensors(ipmi_monitoring_ctx_t ctx, const char *hostname, struct ipmi_monitoring_ipmi_config *config, enum ipmi_monitoring_sensor_units target_units) {
    unsigned int flags = IPMI_MONITORING_SENSOR_READING_FLAGS_DISCRETE_READING;
    int sensor_count;

    sensor_count = ipmi_monitoring_sensor_readings_by_record_id(ctx, hostname, config, flags, NULL, 0, NULL, NULL);

    if (sensor_count < 0) {
        fprintf(stderr, "Power sensor query failed: %s\n", ipmi_monitoring_ctx_errormsg(ctx));
        return NULL;
    }
    //printf("\nPower Sensors\n");
    //printf("Timestamp,RecordID,SensorNumber,SensorName,State,Power_W\n");

    int power_count = 0;
    spec_list *list1 = NULL;

    for (int i = 0; i < sensor_count; i++) {
        int record_id = ipmi_monitoring_sensor_read_record_id(ctx);
        int sensor_number = ipmi_monitoring_sensor_read_sensor_number(ctx);
        int sensor_state = ipmi_monitoring_sensor_read_sensor_state(ctx);
        int sensor_units = ipmi_monitoring_sensor_read_sensor_units(ctx);
        int reading_type = ipmi_monitoring_sensor_read_sensor_reading_type(ctx);

        char *sensor_name = ipmi_monitoring_sensor_read_sensor_name(ctx);
        void *reading = ipmi_monitoring_sensor_read_sensor_reading(ctx);

        if (sensor_units == (int)target_units && reading != NULL && reading_type == IPMI_MONITORING_SENSOR_READING_TYPE_DOUBLE) {
            //print_timestamp();
            //printf(",%d,%d,\"%s\",%s,%.6f\n", record_id, sensor_number, sensor_name ? sensor_name : "", state_string(sensor_state), *((double *)reading));
            power_count++;
            list1 = (spec_list *)realloc(list1, (size_t)(power_count * sizeof(*list1)));
            list1[power_count-1].record_id = record_id;
            list1[power_count-1].sensor_number = sensor_number;
            list1[power_count-1].sensor_state = sensor_state;
            list1[power_count-1].sensor_units = sensor_units;
            list1[power_count-1].reading_type = reading_type;
            list1[power_count-1].name = strdup(sensor_name);
        }
        ipmi_monitoring_sensor_iterator_next(ctx);
    }
    power_sensor_count = power_count;
    return list1;
}

spec_list* get_specific_sensors (ipmi_monitoring_ctx_t ctx, const char *hostname, struct ipmi_monitoring_ipmi_config *config, enum ipmi_monitoring_sensor_units *target_units, int num_units){
    unsigned int flags = IPMI_MONITORING_SENSOR_READING_FLAGS_DISCRETE_READING;
    int sensor_count;
    uint8_t spec_pass;
    sensor_count = ipmi_monitoring_sensor_readings_by_record_id(ctx, hostname, config, flags, NULL, 0, NULL, NULL);
    if (sensor_count < 0){
        fprintf(stderr, "[!F] Power, sensor query failed!\n", ipmi_monitoring_ctx_errormsg(ctx));
        return NULL;
    }
    if (target_units == NULL || num_units <= 0){
        spec_pass = 1;
    } else {
        spec_pass = 0;
    }
    int spec_sensor_count = 0;
    spec_list *list = NULL;
    for (int i = 0; i < sensor_count; ++i){
        int record_id = ipmi_monitoring_sensor_read_record_id(ctx);
        int sensor_number = ipmi_monitoring_sensor_read_sensor_number(ctx);
        int sensor_state = ipmi_monitoring_sensor_read_sensor_state(ctx);
        int sensor_units = ipmi_monitoring_sensor_read_sensor_units(ctx);
        int reading_type = ipmi_monitoring_sensor_read_sensor_reading_type(ctx);
        char *sensor_name = ipmi_monitoring_sensor_read_sensor_name(ctx);
        void *reading = ipmi_monitoring_sensor_read_sensor_reading(ctx);
        if ((!spec_pass) && reading != NULL && reading_type == IPMI_MONITORING_SENSOR_READING_TYPE_DOUBLE){
            for (int j = 0; j < num_units; ++j) {
                if (sensor_units == (int)target_units[j]){
                    spec_sensor_count++;
                    list = (spec_list *)realloc(list, (size_t)(spec_sensor_count * sizeof(*list)));
                    list[spec_sensor_count-1].record_id = record_id;
                    list[spec_sensor_count-1].sensor_number = sensor_number;
                    list[spec_sensor_count-1].sensor_state = sensor_state;
                    list[spec_sensor_count-1].sensor_units = sensor_units;
                    list[spec_sensor_count-1].reading_type = reading_type;
                    list[spec_sensor_count-1].name = strdup(sensor_name);
                } else{
                    continue;
                }
            }
        }
        ipmi_monitoring_sensor_iterator_next(ctx);
    }
    return list;

}

int update_sensor_readings(ipmi_monitoring_ctx_t ctx, const char *hostname, struct ipmi_monitoring_ipmi_config *config, spec_list *list, int power_count){
    unsigned int flags = IPMI_MONITORING_SENSOR_READING_FLAGS_DISCRETE_READING;
    if (list == NULL || power_count <= 0)
        return 0;

    unsigned int *record_ids = malloc((size_t)power_count * sizeof(*record_ids));

    if (record_ids == NULL) {
        fprintf(stderr, "Failed to allocate record ID array\n");
        return -1;
    }

    for (int i = 0; i < power_count; i++)
        record_ids[i] = (unsigned int)list[i].record_id;

    int sensor_count = ipmi_monitoring_sensor_readings_by_record_id(ctx, hostname, config, flags, record_ids, (unsigned int)power_count, NULL, NULL);
    free(record_ids);

    if (sensor_count < 0) {
        fprintf(stderr, "Power sensor reading failed: %s\n",
                ipmi_monitoring_ctx_errormsg(ctx));
        return -1;
    }

    for (int i = 0; i < sensor_count; i++) {
        int record_id = ipmi_monitoring_sensor_read_record_id(ctx);
        //int sensor_state = ipmi_monitoring_sensor_read_sensor_state(ctx);
        //int sensor_units = ipmi_monitoring_sensor_read_sensor_units(ctx);
        //int reading_type = ipmi_monitoring_sensor_read_sensor_reading_type(ctx);
        void *reading = ipmi_monitoring_sensor_read_sensor_reading(ctx);
        void *name = ipmi_monitoring_sensor_read_sensor_name(ctx);

        for (int j = 0; j < power_count; j++) {
            if (list[j].record_id == record_id) {
                list[j].reading = *((double*)reading);
                list[j].name = strdup(name);
                printf("[*] %d, %s, %f\n", record_id, list[j].name, list[j].reading);
                break;
            }
        }

        if (i < sensor_count - 1)
            ipmi_monitoring_sensor_iterator_next(ctx);
    }
    return sensor_count;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <BMC_IP> <USERNAME> <PASSWORD> <INTERVAL_SECONDS>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *hostname = argv[1];
    char *username = argv[2];
    char *password = argv[3];
    printf("[*] argv[4]: %s\n", argv[4]);
    float interval = atof(argv[4]);

    if (interval <= 0) {
        fprintf(stderr, "Sampling interval must be > 0 seconds\n");
        return EXIT_FAILURE;
    }

    int errnum;

    if (ipmi_monitoring_init(0, &errnum) < 0) {
        fprintf(stderr, "ipmi_monitoring_init failed: %s\n", ipmi_monitoring_ctx_strerror(errnum));
        return EXIT_FAILURE;
    }

    ipmi_monitoring_ctx_t ctx = ipmi_monitoring_ctx_create();

    if (!ctx) {
        fprintf(stderr, "Could not create IPMI monitoring context\n");
        return EXIT_FAILURE;
    }

    /*
     * Load the standard FreeIPMI sensor interpretation configuration.
     */
    if (ipmi_monitoring_ctx_sensor_config_file(ctx, NULL) < 0) {
        fprintf(stderr, "Sensor config error: %s\n", ipmi_monitoring_ctx_errormsg(ctx));
        ipmi_monitoring_ctx_destroy(ctx);
        return EXIT_FAILURE;
    }

    struct ipmi_monitoring_ipmi_config config;
    memset(&config, 0, sizeof(config));

    /*
     * These fields are primarily relevant to local/in-band IPMI.
     * -1 asks FreeIPMI to use its default where applicable.
     */
    config.driver_type = -1;
    config.disable_auto_probe = 0;
    config.driver_address = 0;
    config.register_spacing = 0;
    config.driver_device = NULL;

    /*
     * Remote IPMI configuration.
     *
     * -1 lets FreeIPMI select/default the protocol/auth settings.
     * This is useful as a first implementation and tends to work
     * with modern IPMI 2.0 BMCs.
     */
    /*
    config.protocol_version = -1;
    config.username = username;
    config.password = password;

    config.k_g = NULL;
    config.k_g_len = 0;

    config.privilege_level = -1;
    config.authentication_type = -1;
    config.cipher_suite_id = -1;

    config.session_timeout_len = 0;
    config.retransmission_timeout_len = 0;

    config.workaround_flags = 0;
    */
    config.protocol_version = IPMI_MONITORING_PROTOCOL_VERSION_2_0;
    config.username = username;
    config.password = password;

    config.k_g = NULL;
    config.k_g_len = 0;

    config.privilege_level = IPMI_MONITORING_PRIVILEGE_LEVEL_ADMIN;
    config.authentication_type = -1;
    config.cipher_suite_id = 3;

    config.session_timeout_len = 0;
    config.retransmission_timeout_len = 0;
    config.workaround_flags = 0;
    printf("Connecting to BMC: %s\n", hostname);
    printf("Sampling interval: %f seconds\n", interval);

    int first_sample = 1;
    if (sample_sensors(ctx, hostname, &config, first_sample) < 0){
        fprintf(stderr, "[!] Sampling sensors failed!\n");
    }

    spec_list *pwr_sensors = get_power_sensors(ctx, hostname, &config, IPMI_MONITORING_SENSOR_UNITS_WATTS);
    
    while (1) {
        /*
        if (sample_sensors(ctx, hostname, &config, first_sample) < 0) {
            fprintf(stderr, "Sampling cycle failed\n");
        }
        */
        if (update_power_sensor_readings(ctx, hostname, &config, pwr_sensors, power_sensor_count) < 0){
            fprintf(stderr, "[!] sampling cycle failed, update function error!\n");
        }

        printf("\n");
        fflush(stdout);
        sleep(interval);
    }

    ipmi_monitoring_ctx_destroy(ctx);

    return EXIT_SUCCESS;
}