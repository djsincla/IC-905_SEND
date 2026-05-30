/*
 * ic905_relay.c — IC-905 Packet Capture and Relay Sequencer
 *
 * Captures ethernet packets between an Icom IC-905 controller and its RF deck,
 * decodes band selection and TX state, and drives I2C relays (PCA9538A GPIO
 * expanders) as a timed TX/RX sequence defined in /etc/ic905-relay.conf.
 *
 * On TX:  matching relays CLOSE in increasing delay order.
 * On RX:  they OPEN in mirrored reverse order (last closed = first open).
 *
 * Build:  make
 * Run:    sudo ./ic905-relay
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <syslog.h>
#include <time.h>
#include <poll.h>

#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include <pcap/pcap.h>
#include <gpiod.h>
#include <mosquitto.h>
#include <pthread.h>

/* ── Configuration ────────────────────────────────────────────────────── */

#define IC905_VERSION   "1.16"

#define IFACE           "eth0"
#define CAPTURE_FILTER  "dst port 50004"  /* controller->deck stream: heartbeat + the 0x44 status/command frames (band, frequency, TX state) */

#define I2C_BUS         "/dev/i2c-1"
#define BOARD1_ADDR     0x70
#define BOARD2_ADDR     0x73

#define PCA9538A_REG_OUTPUT  0x01
#define PCA9538A_REG_CONFIG  0x03

/* Relay bit masks on PCA9538A output register */
#define RL1_BIT  0x04   /* P2 */
#define RL2_BIT  0x02   /* P1 */
#define RL3_BIT  0x01   /* P0 */

#define NUM_RELAYS  6              /* relays 1-3 = board1 (0x70), 4-6 = board2 (0x73) */
#define MAX_RULES   64        /* multi-band lines expand into one rule per band */
#define MAX_SCHED   (2 * NUM_RELAYS)
#define BAND_ALL    (-1)           /* config token "all": matches any TX band */
#define CONFIG_PATH "/etc/ic905-relay.conf"

/* GPIO lines for PCA9538A hardware reset */
#define GPIO_CHIP       "gpiochip0"
#define RST_BOARD1_PIN  5
#define RST_BOARD2_PIN  12

/* ── Band definitions ─────────────────────────────────────────────────── */

typedef enum {
    BAND_144 = 0,
    BAND_430,
    BAND_1200,
    BAND_2400,
    BAND_5600,
    BAND_10G,
    BAND_COUNT,
    BAND_UNKNOWN
} band_t;

static const char *band_names[] = {
    [BAND_144]     = "144 MHz",
    [BAND_430]     = "430 MHz",
    [BAND_1200]    = "1200 MHz",
    [BAND_2400]    = "2400 MHz",
    [BAND_5600]    = "5600 MHz",
    [BAND_10G]     = "10 GHz",
    [BAND_UNKNOWN] = "Unknown",
};

/* Ham wavelength names, for human-readable log/status lines. */
static const char *band_short[] = {
    [BAND_144]     = "2m",
    [BAND_430]     = "70cm",
    [BAND_1200]    = "23cm",
    [BAND_2400]    = "13cm",
    [BAND_5600]    = "6cm",
    [BAND_10G]     = "3cm",
    [BAND_UNKNOWN] = "?",
};

/* ── Relay hardware mapping (global relay numbers 1-6) ────────────────── */

static const uint8_t relay_board[NUM_RELAYS] = {
    BOARD1_ADDR, BOARD1_ADDR, BOARD1_ADDR,
    BOARD2_ADDR, BOARD2_ADDR, BOARD2_ADDR,
};
static const uint8_t relay_mask[NUM_RELAYS] = {
    RL1_BIT, RL2_BIT, RL3_BIT,
    RL1_BIT, RL2_BIT, RL3_BIT,
};

static inline int board_idx(uint8_t addr) { return addr == BOARD1_ADDR ? 0 : 1; }

/* ── Sequencing rules (loaded from CONFIG_PATH) ───────────────────────── */

typedef struct {
    int relay;       /* 1..NUM_RELAYS */
    int band;        /* band_t value, or BAND_ALL */
    int delay_ms;
} seq_rule_t;

static seq_rule_t rules[MAX_RULES];
static int        n_rules = 0;
static int        board_used[2] = { 0, 0 };   /* [0]=0x70, [1]=0x73 */

/* ── Radio state ──────────────────────────────────────────────────────── */

typedef struct {
    band_t   band;
    int      transmitting;   /* 1 = TX, 0 = RX */
    uint64_t freq;           /* actual on-air RF in Hz = reported IF + per-band offset */
    int      power;          /* TX power %, -1 if unknown */
    band_t   band_b;         /* sub-VFO band (byte 196) */
    uint64_t freq_b;         /* sub-VFO actual RF in Hz */
    int      split;          /* 1 = split enabled (byte 236 bit 7, read when idle) */
    band_t   op_band;        /* OPERATING (transmit) band: split ? sub-VFO(196) : active(184) */
    uint64_t op_freq;        /* operating (transmit) RF in Hz — the sub VFO's when split */
} radio_state_t;

/* ── Scheduled relay events ───────────────────────────────────────────── */

typedef struct {
    struct timespec when;   /* CLOCK_MONOTONIC fire time */
    int             relay;  /* 1..NUM_RELAYS */
    int             on;     /* 1 = close, 0 = open */
} sched_event_t;

static sched_event_t sched[MAX_SCHED];
static int           n_sched = 0;

/* ── Globals ──────────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_running = 1;
static int                   g_i2c_fd  = -1;
static uint8_t               g_board_output[2];      /* [0]=board1, [1]=board2 */
static int                   relay_on[NUM_RELAYS];   /* physical relay state */
static radio_state_t         g_prev_state = { BAND_UNKNOWN, 0, 0, -1, BAND_UNKNOWN, 0, 0, BAND_UNKNOWN, 0 };
static band_t                g_band = BAND_UNKNOWN;  /* last decoded band */
static uint64_t              g_freq = 0;             /* last decoded actual RF (Hz) */
static int                   g_power = -1;           /* last decoded TX power %, -1 = unknown */
static band_t                g_band_b = BAND_UNKNOWN; /* sub-VFO band (byte 196) */
static uint64_t              g_freq_b = 0;            /* sub-VFO actual RF (Hz) */
static int                   g_split = 0;             /* split flag: payload[27] == 1.
                                                          Direct, band-independent (per K7MDL's
                                                          well-validated IC905_Ethernet_Decoder). */
/* Per-band LO offset (MHz): actual RF = reported IF + offset. All confirmed
   on-air against the operator's dial: 2m=0 (the IF IS the true RF), 70cm=199,
   23cm=889, 13cm=1738, 6cm=4687, 3cm=8611. Override any via freq_offset_<band>. */
static int                   g_offset_mhz[BAND_COUNT] = {
    [BAND_144] = 0, [BAND_430] = 199, [BAND_1200] = 889,
    [BAND_2400] = 1738, [BAND_5600] = 4687, [BAND_10G] = 8611,
};
static int                   g_tx_bit = 0;           /* TX bit from the last band frame */
static int                   g_have_band = 0;        /* set once we've decoded a band */
static pcap_t               *g_pcap = NULL;          /* set after pcap setup; used by signal handler */

/* ── MQTT (optional monitoring + control over WiFi) ───────────────────── */

static struct mosquitto *g_mosq = NULL;
static int   g_mqtt_enable = 0;
static char  g_mqtt_broker[128] = "127.0.0.1";
static int   g_mqtt_port = 1883;
static char  g_mqtt_prefix[64] = "ic905";
static char  g_mqtt_user[64] = "";
static char  g_mqtt_pass[64] = "";
static volatile sig_atomic_t g_mqtt_need_republish = 0;

static int   relay_locked[NUM_RELAYS];   /* 1 = held under manual MQTT control (sequencer skips it) */

/* MQTT commands: network thread pushes, RT thread drains — keeps all I2C on
   the RT thread so the relay path is never blocked by MQTT. */
typedef enum { CMD_RELAY_OPEN, CMD_RELAY_CLOSE, CMD_RELAY_AUTO, CMD_MODE_MANUAL, CMD_MODE_AUTO } cmd_kind_t;
typedef struct { cmd_kind_t kind; int relay; } cmd_t;
#define CMD_QLEN 32
static cmd_t           cmd_q[CMD_QLEN];
static int             cmd_head = 0, cmd_tail = 0;
static pthread_mutex_t cmd_mtx = PTHREAD_MUTEX_INITIALIZER;

/* MQTT publish helpers (defined below; safe no-ops when MQTT is disabled) */
static void mqtt_pub_relay(int relay);
static void mqtt_pub_mode(int relay);
static void mqtt_pub_band(void);
static void mqtt_pub_tx(void);
static void mqtt_pub_freq(void);
static void mqtt_pub_power(void);
static void mqtt_pub_status(void);
static void mqtt_pub_band_b(void);
static void mqtt_pub_freq_b(void);
static void mqtt_pub_split(void);
static void mqtt_pub_state(void);

/* ── Signal handling ──────────────────────────────────────────────────── */

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
    if (g_pcap) pcap_breakloop(g_pcap);   /* unblock the poll/dispatch loop for prompt exit */
}

static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/* ── I2C helpers ──────────────────────────────────────────────────────── */

static int i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    struct i2c_msg msgs[1] = {{
        .addr  = addr,
        .flags = 0,
        .len   = 2,
        .buf   = buf,
    }};
    struct i2c_rdwr_ioctl_data data = {
        .msgs  = msgs,
        .nmsgs = 1,
    };

    if (ioctl(g_i2c_fd, I2C_RDWR, &data) < 0) {
        syslog(LOG_ERR, "I2C write 0x%02X reg 0x%02X = 0x%02X failed: %s",
               addr, reg, val, strerror(errno));
        return -1;
    }
    return 0;
}

/* Recompose and write a board's output register from current relay states. */
static void write_board(int bidx)
{
    uint8_t addr = (bidx == 0) ? BOARD1_ADDR : BOARD2_ADDR;
    uint8_t out  = 0x00;
    for (int r = 0; r < NUM_RELAYS; r++)
        if (relay_on[r] && board_idx(relay_board[r]) == bidx)
            out |= relay_mask[r];
    g_board_output[bidx] = out;
    /* Deliberate log-and-continue: i2c_write_reg already logs failures; we keep
       the in-memory state and carry on (a transient bus error self-corrects on
       the next edge / the GPIO reset at startup). */
    i2c_write_reg(addr, PCA9538A_REG_OUTPUT, out);
}

/* Set one relay (1..NUM_RELAYS) on/off and update its board register. */
static void set_relay(int relay, int on)
{
    if (relay < 1 || relay > NUM_RELAYS) return;
    int idx = relay - 1;
    if (relay_on[idx] == on) return;
    relay_on[idx] = on;
    int bidx = board_idx(relay_board[idx]);
    if (board_used[bidx])
        write_board(bidx);
    syslog(LOG_INFO, "relay %d %s (board 0x%02X, bit 0x%02X)",
           relay, on ? "CLOSE" : "open", relay_board[idx], relay_mask[idx]);
    mqtt_pub_relay(relay);   /* non-blocking, after the I2C write */
    mqtt_pub_state();
}

static void open_all_relays(void)
{
    for (int r = 0; r < NUM_RELAYS; r++) relay_on[r] = 0;
    if (board_used[0]) { g_board_output[0] = 0; i2c_write_reg(BOARD1_ADDR, PCA9538A_REG_OUTPUT, 0); }
    if (board_used[1]) { g_board_output[1] = 0; i2c_write_reg(BOARD2_ADDR, PCA9538A_REG_OUTPUT, 0); }
}

static int i2c_init(void)
{
    g_i2c_fd = open(I2C_BUS, O_RDWR);
    if (g_i2c_fd < 0) {
        syslog(LOG_ERR, "Failed to open %s: %s", I2C_BUS, strerror(errno));
        return -1;
    }

    /* Drive every relay OPEN *before* enabling the outputs. The PCA9538A output
       register powers up at 0xFF, so setting the config register (P0-P2 =
       outputs) first would briefly drive the pins HIGH and close every relay
       until the output write lands. Clearing the output register while the pins
       are still inputs (high-Z), then switching them to outputs, makes them go
       straight to LOW (open) with no transient — relays never twitch at startup.
       Only touch boards that have relays mapped; others are left untouched. */
    for (int r = 0; r < NUM_RELAYS; r++) relay_on[r] = 0;
    if (board_used[0]) { g_board_output[0] = 0; if (i2c_write_reg(BOARD1_ADDR, PCA9538A_REG_OUTPUT, 0x00) < 0) return -1; }
    if (board_used[1]) { g_board_output[1] = 0; if (i2c_write_reg(BOARD2_ADDR, PCA9538A_REG_OUTPUT, 0x00) < 0) return -1; }

    /* Now enable P0-P2 as outputs (0 = output), P3-P7 as inputs (1 = input);
       the pins immediately drive the 0x00 we just set, so all relays stay open. */
    if (board_used[0] && i2c_write_reg(BOARD1_ADDR, PCA9538A_REG_CONFIG, 0xF8) < 0) return -1;
    if (board_used[1] && i2c_write_reg(BOARD2_ADDR, PCA9538A_REG_CONFIG, 0xF8) < 0) return -1;

    syslog(LOG_INFO, "PCA9538A initialized (boards:%s%s)",
           board_used[0] ? " 0x70" : "", board_used[1] ? " 0x73" : "");
    return 0;
}

/* ── GPIO reset ───────────────────────────────────────────────────────── */

static int gpio_reset_pin(struct gpiod_chip *chip, unsigned int pin)
{
    struct gpiod_line *line = gpiod_chip_get_line(chip, pin);
    if (!line) {
        syslog(LOG_ERR, "Failed to get GPIO line %u: %s", pin, strerror(errno));
        return -1;
    }
    if (gpiod_line_request_output(line, "ic905-relay", 1) < 0) {
        syslog(LOG_ERR, "Failed to request GPIO %u: %s", pin, strerror(errno));
        return -1;
    }
    gpiod_line_set_value(line, 0);   /* assert reset  */
    usleep(100000);
    gpiod_line_set_value(line, 1);   /* release reset */
    usleep(100000);                  /* settling time */
    gpiod_line_release(line);
    return 0;
}

/* Pulse the PCA9538A reset only on boards we actually drive; boards with no
   mapped relays are left untouched. */
static int gpio_reset_boards(void)
{
    struct gpiod_chip *chip = gpiod_chip_open_by_name(GPIO_CHIP);
    if (!chip) {
        syslog(LOG_ERR, "Failed to open GPIO chip %s: %s", GPIO_CHIP, strerror(errno));
        return -1;
    }

    int rc = 0;
    if (board_used[0] && gpio_reset_pin(chip, RST_BOARD1_PIN) < 0) rc = -1;
    if (board_used[1] && gpio_reset_pin(chip, RST_BOARD2_PIN) < 0) rc = -1;

    gpiod_chip_close(chip);

    if (rc == 0)
        syslog(LOG_INFO, "PCA9538A reset complete (boards:%s%s)",
               board_used[0] ? " 0x70" : "", board_used[1] ? " 0x73" : "");
    return rc;
}

/* ── Config ───────────────────────────────────────────────────────────── */

static band_t band_from_token(const char *t)
{
    if (!strcasecmp(t, "2m")   || !strcmp(t, "144"))  return BAND_144;
    if (!strcasecmp(t, "70cm") || !strcmp(t, "430"))  return BAND_430;
    if (!strcasecmp(t, "23cm") || !strcmp(t, "1200")) return BAND_1200;
    if (!strcasecmp(t, "13cm") || !strcmp(t, "2400")) return BAND_2400;
    if (!strcasecmp(t, "6cm")  || !strcmp(t, "5600")) return BAND_5600;
    if (!strcasecmp(t, "3cm")  || !strcasecmp(t, "10g") || !strcmp(t, "10000")) return BAND_10G;
    return BAND_UNKNOWN;
}

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = '\0';
    return s;
}

/* Parse CONFIG_PATH into rules[]/board_used[].  Each non-comment line is
   "<relay>, <band>, <delay_ms>".  '#' starts a comment. */
static void load_config(const char *path)
{
    n_rules = 0;
    board_used[0] = board_used[1] = 0;

    FILE *f = fopen(path, "r");
    if (!f) {
        syslog(LOG_ERR, "Cannot open config %s: %s — no relays mapped",
               path, strerror(errno));
        return;
    }

    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        if (!*trim(line)) continue;

        /* key = value directive (MQTT settings) — relay rules use commas,
           directives use '=', so distinguish on that. */
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            char *key = trim(line);
            char *val = trim(eq + 1);
            if      (!strcasecmp(key, "mqtt_enable")) g_mqtt_enable = atoi(val);
            else if (!strcasecmp(key, "mqtt_broker")) snprintf(g_mqtt_broker, sizeof g_mqtt_broker, "%s", val);
            else if (!strcasecmp(key, "mqtt_port"))   g_mqtt_port = atoi(val);
            else if (!strcasecmp(key, "mqtt_prefix")) snprintf(g_mqtt_prefix, sizeof g_mqtt_prefix, "%s", val);
            else if (!strcasecmp(key, "mqtt_user"))   snprintf(g_mqtt_user, sizeof g_mqtt_user, "%s", val);
            else if (!strcasecmp(key, "mqtt_pass"))   snprintf(g_mqtt_pass, sizeof g_mqtt_pass, "%s", val);
            else if (!strncasecmp(key, "freq_offset_", 12)) {
                band_t b = band_from_token(key + 12);
                if (b < BAND_COUNT) g_offset_mhz[b] = atoi(val);
                else syslog(LOG_WARNING, "config line %d: unknown band in '%s'", lineno, key);
            }
            else syslog(LOG_WARNING, "config line %d: unknown directive '%s'", lineno, key);
            continue;
        }

        /* three comma-separated fields: relay, band, delay */
        char *f1 = line;
        char *f2 = strchr(f1, ',');
        if (!f2) { syslog(LOG_WARNING, "config line %d: expected 'relay, band, delay'", lineno); continue; }
        *f2++ = '\0';
        char *f3 = strchr(f2, ',');
        if (!f3) { syslog(LOG_WARNING, "config line %d: expected 'relay, band, delay'", lineno); continue; }
        *f3++ = '\0';

        f1 = trim(f1); f2 = trim(f2); f3 = trim(f3);

        int relay = atoi(f1);
        if (relay < 1 || relay > NUM_RELAYS) {
            syslog(LOG_WARNING, "config line %d: relay %d out of range 1-%d", lineno, relay, NUM_RELAYS);
            continue;
        }

        /* delays: '/'-separated list — a single value is shared by all bands */
        int delays[NUM_RELAYS], ndelays = 0;
        for (char *dtok = strtok(f3, "/"); dtok && ndelays < NUM_RELAYS; dtok = strtok(NULL, "/")) {
            int v = atoi(trim(dtok));
            delays[ndelays++] = v < 0 ? 0 : v;
        }
        if (ndelays == 0) { syslog(LOG_WARNING, "config line %d: missing delay", lineno); continue; }

        /* bands: '/'-separated list — emit one rule per band */
        int bi = 0;
        for (char *btok = strtok(f2, "/"); btok; btok = strtok(NULL, "/"), bi++) {
            char *bt = trim(btok);
            int band;
            if (!strcasecmp(bt, "all")) {
                band = BAND_ALL;
            } else {
                band_t b = band_from_token(bt);
                if (b == BAND_UNKNOWN || b >= BAND_COUNT) {
                    syslog(LOG_WARNING, "config line %d: unknown band '%s'", lineno, bt);
                    continue;
                }
                band = (int)b;
            }
            int delay = (ndelays == 1) ? delays[0]
                      : (bi < ndelays ? delays[bi] : delays[ndelays - 1]);

            if (n_rules >= MAX_RULES) {
                syslog(LOG_WARNING, "config: too many rules (max %d), ignoring rest", MAX_RULES);
                break;
            }
            rules[n_rules].relay    = relay;
            rules[n_rules].band     = band;
            rules[n_rules].delay_ms = delay;
            n_rules++;
            board_used[board_idx(relay_board[relay - 1])] = 1;

            syslog(LOG_INFO, "config: relay %d, band %s, delay %d ms",
                   relay, band == BAND_ALL ? "all" : band_names[band], delay);
        }
    }
    fclose(f);
    syslog(LOG_INFO, "Loaded %d rule(s) from %s; boards in use:%s%s",
           n_rules, path, board_used[0] ? " 0x70" : "", board_used[1] ? " 0x73" : "");
}

/* delay for (relay, band): exact band rule wins, else ALL rule, else -1 (n/a). */
static int delay_for(int relay, band_t band)
{
    int all_delay = -1;
    for (int i = 0; i < n_rules; i++) {
        if (rules[i].relay != relay) continue;
        if (rules[i].band == (int)band) return rules[i].delay_ms;
        if (rules[i].band == BAND_ALL)  all_delay = rules[i].delay_ms;
    }
    return all_delay;
}

/* ── Scheduler ────────────────────────────────────────────────────────── */

static void sched_clear(void) { n_sched = 0; }

static void sched_add(int offset_ms, int relay, int on)
{
    if (n_sched >= MAX_SCHED) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ns  = now.tv_nsec + (long)(offset_ms % 1000) * 1000000L;
    time_t s = now.tv_sec + offset_ms / 1000;
    if (ns >= 1000000000L) { ns -= 1000000000L; s++; }
    sched[n_sched].when.tv_sec  = s;
    sched[n_sched].when.tv_nsec = ns;
    sched[n_sched].relay = relay;
    sched[n_sched].on    = on;
    n_sched++;
}

/* ms until the soonest pending event (>=0), or -1 if none. */
static int next_timeout_ms(void)
{
    if (n_sched == 0) return -1;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long best = -1;
    for (int i = 0; i < n_sched; i++) {
        long ms = (sched[i].when.tv_sec - now.tv_sec) * 1000L
                + (sched[i].when.tv_nsec - now.tv_nsec) / 1000000L;
        if (ms < 0) ms = 0;
        if (best < 0 || ms < best) best = ms;
    }
    return (int)best;
}

/* Fire all events whose time has arrived; compact the queue. */
static void fire_due_events(void)
{
    if (n_sched == 0) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int w = 0;
    for (int i = 0; i < n_sched; i++) {
        int due = (sched[i].when.tv_sec < now.tv_sec) ||
                  (sched[i].when.tv_sec == now.tv_sec && sched[i].when.tv_nsec <= now.tv_nsec);
        if (due)
            set_relay(sched[i].relay, sched[i].on);
        else
            sched[w++] = sched[i];
    }
    n_sched = w;
}

/* ── Re-sequence on a TX/band edge ────────────────────────────────────── */

/*
 * Rebuild the relay schedule from the current physical state to the new
 * target. One rule covers every case (TX on, TX off, band change mid-TX,
 * fast keying): clear pending events, then ramp down what is no longer
 * wanted (mirrored), then ramp up what is newly wanted.
 */
static void reseq(radio_state_t prev, radio_state_t curr)
{
    sched_clear();

    /* relays wanted closed now (manually-locked relays are skipped entirely) */
    int want[NUM_RELAYS] = { 0 };
    if (curr.transmitting)
        for (int r = 1; r <= NUM_RELAYS; r++)
            if (!relay_locked[r - 1] && delay_for(r, curr.op_band) >= 0)
                want[r - 1] = 1;

    band_t pband = (prev.op_band < BAND_COUNT) ? prev.op_band : BAND_UNKNOWN;

    /* down-set max delay (using the previous band) for mirroring */
    int dmax = 0, have_down = 0;
    for (int r = 1; r <= NUM_RELAYS; r++) {
        if (relay_locked[r - 1]) continue;
        if (relay_on[r - 1] && !want[r - 1]) {
            have_down = 1;
            int d = delay_for(r, pband);
            if (d < 0) d = 0;
            if (d > dmax) dmax = d;
        }
    }

    /* down-ramp (mirror): last closed opens first */
    for (int r = 1; r <= NUM_RELAYS; r++) {
        if (relay_locked[r - 1]) continue;
        if (relay_on[r - 1] && !want[r - 1]) {
            int d = delay_for(r, pband);
            if (d < 0) d = 0;
            sched_add(dmax - d, r, 0);
        }
    }

    /* up-ramp begins after the down-ramp completes */
    int base = have_down ? dmax : 0;
    for (int r = 1; r <= NUM_RELAYS; r++) {
        if (relay_locked[r - 1]) continue;
        if (want[r - 1] && !relay_on[r - 1]) {
            int d = delay_for(r, curr.op_band);
            if (d < 0) d = 0;
            sched_add(base + d, r, 1);
        }
    }
}

/* ── Packet decode ────────────────────────────────────────────────────── */

/*
 * Field offsets in the controller->deck 0x44 status frame (verified against a
 * live 2m transmit capture, all bands share the layout):
 *   frequency: 4-byte little-endian uint32 at offset 184 from payload start
 *   TX state:  byte 38 (1 = transmitting, 0 = receiving)
 */
#define BAND_OFFSET_FROM_START  184  /* band frequency field: fixed offset from payload start (header), constant across frame sizes */
#define BAND_BYTE_LEN           4

/* The controller->deck status/command frame (payload[0]==0x01, payload[10]==0x44)
   carries the explicit TX command at byte 38 (1=TX, 0=RX), sent at every key
   edge for every band; its larger frames also carry the band frequency above. */
#define CMD_TYPE_OFFSET  0
#define CMD_MSG_OFFSET   10
#define CMD_MSG_ID       0x44
#define TX_FLAG_OFFSET   38
#define POWER_OFFSET     236  /* TX power byte (start offset); only in the full ~240B frame */

/*
 * Map the reported frequency field to a band. The field is the TRUE frequency
 * only for 2m (144 MHz); every higher band reports an IF value the radio
 * up-converts from. Measured on-air 2026-05-26 (one tuned point per band):
 *   2m   144.1 MHz   70cm 233.1 MHz   23cm 407.0 MHz
 *   13cm 566.1 MHz   6cm 1073.0 MHz   3cm  1757.3 MHz
 * Boundaries are set to the midpoints between adjacent IF values so each band
 * sits well clear of its edges (doppler/tuning shift the IF within a band).
 */
static band_t freq_to_band(uint32_t freq)
{
    if (freq <  189000000) return BAND_144;    /* 2m   IF 144.1, mid(144,233) */
    if (freq <  320000000) return BAND_430;    /* 70cm IF 233.1, mid(233,407) */
    if (freq <  487000000) return BAND_1200;   /* 23cm IF 407.0, mid(407,566) */
    if (freq <  820000000) return BAND_2400;   /* 13cm IF 566.1, mid(566,1073) */
    if (freq < 1415000000) return BAND_5600;   /* 6cm  IF 1073.0, mid(1073,1757) */
    if (freq < 3000000000) return BAND_10G;    /* 3cm  IF 1757.3, generous top for band width */
    return BAND_UNKNOWN;
}

static radio_state_t decode_payload(const uint8_t *payload, int len)
{
    radio_state_t state = { BAND_UNKNOWN, 0, 0, -1, BAND_UNKNOWN, 0, 0, BAND_UNKNOWN, 0 };

    /* TX state: byte 38 of the status/command frame — the controller's explicit
       transmit command to the RF deck (1 = TX, 0 = RX). Sent at every key edge,
       for every band (the only TX indicator the lower bands give). */
    if (len > TX_FLAG_OFFSET)
        state.transmitting = (payload[TX_FLAG_OFFSET] == 1) ? 1 : 0;

    /* Band frequency: 4-byte little-endian uint32 at a FIXED offset from the
       START of the payload — present only in the larger frames of this family
       (the small key-edge command frames carry TX but no frequency). */
    if (len >= BAND_OFFSET_FROM_START + BAND_BYTE_LEN) {
        uint32_t rawif;
        memcpy(&rawif, &payload[BAND_OFFSET_FROM_START], sizeof(rawif)); /* LE field on a LE host (aarch64) */
        state.band = freq_to_band(rawif);   /* band is classified from the raw IF */

        /* The field is the radio's IF (the true RF only on 2m). Add the per-band
           LO offset to recover the actual on-air RF. The IF tracks the dial 1:1,
           so this is exact to the Hz — verified 23cm: IF 407.117 + 889 MHz =
           1296.117 MHz. 64-bit because 6cm/3cm RF exceeds 32 bits. */
        state.freq = (uint64_t)rawif +
            (state.band < BAND_COUNT ? (uint64_t)g_offset_mhz[state.band] * 1000000ull : 0);

        /* TX power %: byte 236 of the full (~240B) frame is the forward-power meter
           DURING TX; when idle that byte carries the split flag (bit 7) instead, so
           only read power on TX frames (verified 23cm 0x40=25%, 3cm 0x1a=10%). */
        if (len > POWER_OFFSET && state.transmitting)
            state.power = (payload[POWER_OFFSET] * 100 + 127) / 255;
    }

    return state;
}

/* Format a frequency in Hz as MHz.kHz.Hz, e.g. 144375004 -> "144.375.004". */
static const char *fmt_freq(uint64_t hz, char *buf, size_t n)
{
    snprintf(buf, n, "%llu.%03llu.%03llu",
             (unsigned long long)(hz / 1000000ull),
             (unsigned long long)((hz / 1000ull) % 1000ull),
             (unsigned long long)(hz % 1000ull));
    return buf;
}

/* ── pcap callback ────────────────────────────────────────────────────── */

/*
 * Recompute (band, TX) from the latest decoded status frame and act on any
 * change. TX follows the controller's explicit transmit command (byte 38 of
 * the 0x44 status frame); band/frequency come from the same frame family.
 * Called from packet_handler and once per main-loop pass.
 */
static void apply_state(void)
{
    if (!g_have_band) return;

    radio_state_t curr;
    curr.band = g_band;
    curr.freq = g_freq;
    /* TX is the controller's explicit transmit command — byte 38 of the 0x44
       status frame — latched in packet_handler. Works for every band. */
    curr.transmitting = g_tx_bit;
    curr.power = g_power;
    curr.band_b = g_band_b;
    curr.freq_b = g_freq_b;
    /* In split the IC-905 transmits on the unselected (secondary) VFO. Split is
       indicated directly by byte 27 == 1 (latched in g_split, per K7MDL's
       IC905_Ethernet_Decoder). The relays MUST sequence the actual TX band:
          split ON  +  valid secondary  ->  TX VFO = byte 196 (secondary)
          otherwise                     ->  TX VFO = byte 184 (primary/displayed)
       This is direct, band-independent, and handles same-band split correctly —
       we just use byte 196 in split, no frequency-comparison guesswork. */
    curr.split = g_split;
    if (curr.split && curr.band_b < BAND_COUNT) {
        curr.op_band = curr.band_b;
        curr.op_freq = curr.freq_b;
    } else {
        curr.op_band = curr.band;
        curr.op_freq = curr.freq;
    }

    int bt_changed    = (curr.op_band != g_prev_state.op_band ||
                         curr.transmitting != g_prev_state.transmitting);
    int freq_changed  = (curr.freq != g_prev_state.freq);
    int power_changed = (curr.power != g_prev_state.power);
    int sub_changed   = (curr.band_b != g_prev_state.band_b || curr.freq_b != g_prev_state.freq_b);
    int split_changed = (curr.split != g_prev_state.split);
    if (!bt_changed && !freq_changed && !power_changed && !sub_changed && !split_changed) return;

    band_t cb = curr.band < BAND_COUNT ? curr.band : BAND_UNKNOWN;
    char fbuf[24];
    fmt_freq(curr.freq, fbuf, sizeof fbuf);
    if (curr.band != g_prev_state.band) {
        band_t pb = g_prev_state.band < BAND_COUNT ? g_prev_state.band : BAND_UNKNOWN;
        syslog(LOG_INFO, "Band: %s -> %s (%s MHz)", band_short[pb], band_short[cb], fbuf);
    }
    if (curr.transmitting != g_prev_state.transmitting) {
        band_t ob = curr.op_band < BAND_COUNT ? curr.op_band : BAND_UNKNOWN;
        char obuf[24]; fmt_freq(curr.op_freq, obuf, sizeof obuf);
        syslog(LOG_INFO, "TX: %s  %s  %s MHz%s", curr.transmitting ? "ON" : "OFF",
               band_short[ob], obuf, curr.split ? "  [split: sub VFO]" : "");
    }
    if (split_changed) {
        band_t sbb = curr.band_b < BAND_COUNT ? curr.band_b : BAND_UNKNOWN;
        char sbuf[24]; fmt_freq(curr.freq_b, sbuf, sizeof sbuf);
        syslog(LOG_INFO, "Split: %s  (sub %s %s MHz)", curr.split ? "ON" : "OFF",
               band_short[sbb], sbuf);
    }

    radio_state_t prev = g_prev_state;
    g_prev_state = curr;

    if (bt_changed) reseq(prev, curr);
    if (bt_changed) { mqtt_pub_band(); mqtt_pub_tx(); }
    if (sub_changed)   { mqtt_pub_band_b(); mqtt_pub_freq_b(); }
    if (split_changed) mqtt_pub_split();
    mqtt_pub_freq();
    mqtt_pub_power();
    mqtt_pub_status();
    mqtt_pub_state();
}

static void packet_handler(u_char *user, const struct pcap_pkthdr *hdr,
                           const u_char *pkt)
{
    (void)user;

    if (hdr->caplen < 14 + 20 + 20)  /* minimum Ethernet+IP+TCP */
        return;
    const uint8_t *ip = pkt + 14;
    int ip_hdr_len = (ip[0] & 0x0F) * 4;
    if (hdr->caplen < (unsigned)(14 + ip_hdr_len + 20))
        return;
    const uint8_t *tcp = ip + ip_hdr_len;
    int tcp_hdr_len = ((tcp[12] >> 4) & 0x0F) * 4;
    int header_total = 14 + ip_hdr_len + tcp_hdr_len;
    if ((int)hdr->caplen < header_total)
        return;
    const uint8_t *payload = tcp + tcp_hdr_len;
    int payload_len = hdr->caplen - header_total;

    /* Decode the 0x44 status/command frame: TX state from byte 38 (every band),
       and band/frequency from offset 184 (its larger frames only). Identify it
       by signature, not size, so every band's frame is decoded. */
    if (payload_len > TX_FLAG_OFFSET &&
        payload[CMD_TYPE_OFFSET] == 0x01 && payload[CMD_MSG_OFFSET] == CMD_MSG_ID) {
        radio_state_t s = decode_payload(payload, payload_len);
        /* byte 184 is the ACTIVE VFO and stays put across a transmission. While
           transmitting, ignore a NON-transmitting frame reporting a different band
           (a transient dual-watch/scan artifact) so it can't disturb the latched
           active band. The actual transmit band is derived from active + sub-VFO +
           split in apply_state — in split the radio keys the SUB VFO (byte 196). */
        if (g_tx_bit && !s.transmitting && s.band != BAND_UNKNOWN && s.band != g_band) {
            /* ignore spurious sub-VFO (RX) frame during TX */
        } else {
            g_tx_bit = s.transmitting;
            if (s.band != BAND_UNKNOWN) {
                if (s.band != g_band) g_power = -1;   /* new band: forget power until a full frame */
                g_band      = s.band;
                g_freq      = s.freq;
                g_have_band = 1;
                if (s.power >= 0) g_power = s.power;  /* power only from TX full frames */
            }
            /* Full-status frames (>= 200 bytes) carry the sub-VFO frequency at byte
               196 ("unselected VFO" in K7MDL's terms) and the split flag at byte 27:
               payload[27] == 1 means split is enabled. byte 27 is a direct, band-
               independent flag (per K7MDL's IC905_Ethernet_Decoder, validated against
               the same controller↔RF deck stream for a long time) — no frequency
               comparison needed, and it handles same-band split correctly because the
               TX VFO is then simply the secondary (byte 196), no guesswork. */
            if (payload_len >= 200) {
                uint32_t subif = payload[196] | (payload[197] << 8) |
                                 (payload[198] << 16) | ((uint32_t)payload[199] << 24);
                band_t sb = freq_to_band(subif);
                if (sb != BAND_UNKNOWN) {
                    g_band_b = sb;
                    g_freq_b = (uint64_t)subif + (uint64_t)g_offset_mhz[sb] * 1000000ull;
                }
                g_split = (payload[27] == 1) ? 1 : 0;
            }
        }
    }

    apply_state();
}

/* ── pcap setup ───────────────────────────────────────────────────────── */

static pcap_t *setup_pcap(const char *iface)
{
    char errbuf[PCAP_ERRBUF_SIZE];
    struct bpf_program fp;

    pcap_t *handle = pcap_create(iface, errbuf);
    if (!handle) {
        syslog(LOG_ERR, "pcap_create(%s): %s", iface, errbuf);
        return NULL;
    }

    pcap_set_snaplen(handle, 512);
    pcap_set_promisc(handle, 1);           /* tap delivers frames addressed to the radio's MACs */
    pcap_set_timeout(handle, 1);           /* 1ms poll timeout */
    pcap_set_immediate_mode(handle, 1);    /* bypass kernel buffering */
    pcap_set_buffer_size(handle, 65536);

    int ret = pcap_activate(handle);
    if (ret < 0) {
        syslog(LOG_ERR, "pcap_activate: %s", pcap_geterr(handle));
        pcap_close(handle);
        return NULL;
    }
    if (ret > 0)
        syslog(LOG_WARNING, "pcap_activate warning: %s", pcap_geterr(handle));

    if (pcap_compile(handle, &fp, CAPTURE_FILTER, 1, PCAP_NETMASK_UNKNOWN) < 0) {
        syslog(LOG_ERR, "pcap_compile: %s", pcap_geterr(handle));
        pcap_close(handle);
        return NULL;
    }

    if (pcap_setfilter(handle, &fp) < 0) {
        syslog(LOG_ERR, "pcap_setfilter: %s", pcap_geterr(handle));
        pcap_freecode(&fp);
        pcap_close(handle);
        return NULL;
    }
    pcap_freecode(&fp);

    syslog(LOG_INFO, "Capturing on %s, filter: %s", iface, CAPTURE_FILTER);
    return handle;
}

/* ── MQTT helpers (RT thread publishes; net thread only subscribes/queues) ─ */

static void mqtt_pub(const char *subtopic, const char *payload, int retain)
{
    if (!g_mosq) return;
    char topic[160];
    snprintf(topic, sizeof topic, "%s/%s", g_mqtt_prefix, subtopic);
    mosquitto_publish(g_mosq, NULL, topic, (int)strlen(payload), payload, 0, retain);
}

static void mqtt_pub_relay(int relay)
{
    if (!g_mosq || relay < 1 || relay > NUM_RELAYS) return;
    char sub[32];
    snprintf(sub, sizeof sub, "relay/%d", relay);
    mqtt_pub(sub, relay_on[relay - 1] ? "close" : "open", 1);
}

static void mqtt_pub_mode(int relay)
{
    if (!g_mosq || relay < 1 || relay > NUM_RELAYS) return;
    char sub[40];
    snprintf(sub, sizeof sub, "relay/%d/mode", relay);
    mqtt_pub(sub, relay_locked[relay - 1] ? "manual" : "auto", 1);
}

static void mqtt_pub_band(void)
{
    band_t b = g_prev_state.band < BAND_COUNT ? g_prev_state.band : BAND_UNKNOWN;
    mqtt_pub("band", band_short[b], 1);   /* ham wavelength name, e.g. "23cm" */
}

static void mqtt_pub_tx(void)
{
    char buf[48];
    if (g_prev_state.transmitting) {
        band_t ob = g_prev_state.op_band < BAND_COUNT ? g_prev_state.op_band : BAND_UNKNOWN;
        char obuf[24]; fmt_freq(g_prev_state.op_freq, obuf, sizeof obuf);
        /* the band + actual RF being transmitted (the SUB VFO when split) + power */
        if (g_prev_state.power >= 0)
            snprintf(buf, sizeof buf, "ON %s %s %d%%", band_short[ob], obuf, g_prev_state.power);
        else
            snprintf(buf, sizeof buf, "ON %s %s", band_short[ob], obuf);
    } else {
        snprintf(buf, sizeof buf, "OFF");
    }
    mqtt_pub("tx", buf, 1);
}

static void mqtt_pub_freq(void)
{
    if (!g_mosq) return;
    char buf[24];
    fmt_freq(g_prev_state.freq, buf, sizeof buf);
    mqtt_pub("freq", buf, 1);   /* actual on-air RF, MHz.kHz.Hz (e.g. 1296.117.007) */
}

static void mqtt_pub_power(void)
{
    if (!g_mosq) return;
    char buf[16];
    if (g_prev_state.power >= 0)
        snprintf(buf, sizeof buf, "%d", g_prev_state.power);   /* TX power % */
    else
        snprintf(buf, sizeof buf, "unknown");                  /* band sent no full frame */
    mqtt_pub("power", buf, 1);
}

/* Liveness + current power on the status topic: "online 50%" / "online" / "offline".
   (The last-will publishes "offline" on disconnect.) */
static void mqtt_pub_status(void)
{
    if (!g_mosq) return;
    char buf[24];
    if (g_prev_state.power >= 0)
        snprintf(buf, sizeof buf, "online %d%%", g_prev_state.power);
    else
        snprintf(buf, sizeof buf, "online");
    mqtt_pub("status", buf, 1);
}

static void mqtt_pub_band_b(void)
{
    band_t b = g_prev_state.band_b < BAND_COUNT ? g_prev_state.band_b : BAND_UNKNOWN;
    mqtt_pub("band_b", band_short[b], 1);   /* sub-VFO wavelength */
}

static void mqtt_pub_freq_b(void)
{
    if (!g_mosq) return;
    char buf[24];
    fmt_freq(g_prev_state.freq_b, buf, sizeof buf);
    mqtt_pub("freq_b", buf, 1);             /* sub-VFO actual RF, MHz.kHz.Hz */
}

static void mqtt_pub_split(void)
{
    mqtt_pub("split", g_prev_state.split ? "on" : "off", 1);
}

static void mqtt_pub_state(void)
{
    if (!g_mosq) return;
    band_t b  = g_prev_state.band   < BAND_COUNT ? g_prev_state.band   : BAND_UNKNOWN;
    band_t bb = g_prev_state.band_b < BAND_COUNT ? g_prev_state.band_b : BAND_UNKNOWN;
    char buf[384];
    char fbuf[24], fbbuf[24];
    fmt_freq(g_prev_state.freq,   fbuf,  sizeof fbuf);
    fmt_freq(g_prev_state.freq_b, fbbuf, sizeof fbbuf);
    int n = snprintf(buf, sizeof buf,
                     "{\"band\":\"%s\",\"freq\":\"%s\",\"tx\":%d,\"power\":%d,\"split\":%d,\"band_b\":\"%s\",\"freq_b\":\"%s\",\"relays\":[",
                     band_short[b], fbuf, g_prev_state.transmitting, g_prev_state.power,
                     g_prev_state.split, band_short[bb], fbbuf);
    for (int r = 0; r < NUM_RELAYS && n < (int)sizeof buf; r++)
        n += snprintf(buf + n, sizeof buf - n, "%s%d", r ? "," : "", relay_on[r]);
    if (n < (int)sizeof buf)
        n += snprintf(buf + n, sizeof buf - n, "],\"modes\":[");
    for (int r = 0; r < NUM_RELAYS && n < (int)sizeof buf; r++)
        n += snprintf(buf + n, sizeof buf - n, "%s\"%s\"", r ? "," : "",
                      relay_locked[r] ? "manual" : "auto");
    if (n < (int)sizeof buf)
        snprintf(buf + n, sizeof buf - n, "]}");
    mqtt_pub("state", buf, 1);
}

/* Set a relay to what the current TX/band state dictates (used on auto-release). */
static void reconcile_relay(int relay)
{
    int desired = (g_prev_state.transmitting &&
                   delay_for(relay, g_prev_state.op_band) >= 0) ? 1 : 0;
    set_relay(relay, desired);
}

/* RT thread: apply queued MQTT commands (all I2C stays on this thread). */
static void cmd_drain(void)
{
    for (;;) {
        cmd_t c;
        pthread_mutex_lock(&cmd_mtx);
        if (cmd_tail == cmd_head) { pthread_mutex_unlock(&cmd_mtx); return; }
        c = cmd_q[cmd_tail];
        cmd_tail = (cmd_tail + 1) % CMD_QLEN;
        pthread_mutex_unlock(&cmd_mtx);

        switch (c.kind) {
        case CMD_RELAY_CLOSE:
        case CMD_RELAY_OPEN: {
            int on = (c.kind == CMD_RELAY_CLOSE);
            if (g_prev_state.transmitting)
                syslog(LOG_WARNING, "MQTT manual relay %d %s during TX (hot-switch risk)",
                       c.relay, on ? "close" : "open");
            relay_locked[c.relay - 1] = 1;
            set_relay(c.relay, on);
            mqtt_pub_mode(c.relay);
            break;
        }
        case CMD_RELAY_AUTO:
            relay_locked[c.relay - 1] = 0;
            reconcile_relay(c.relay);
            mqtt_pub_mode(c.relay);
            break;
        case CMD_MODE_MANUAL:
            for (int r = 1; r <= NUM_RELAYS; r++) { relay_locked[r - 1] = 1; mqtt_pub_mode(r); }
            syslog(LOG_INFO, "MQTT: all relays manual (sequencer frozen)");
            mqtt_pub_state();
            break;
        case CMD_MODE_AUTO:
            for (int r = 1; r <= NUM_RELAYS; r++) { relay_locked[r - 1] = 0; reconcile_relay(r); mqtt_pub_mode(r); }
            syslog(LOG_INFO, "MQTT: all relays auto");
            mqtt_pub_state();
            break;
        }
    }
}

/* RT thread: publish full current state (called after a (re)connect). */
static void mqtt_republish_all(void)
{
    if (!g_mosq) return;
    mqtt_pub_status();
    mqtt_pub_band();
    mqtt_pub_tx();
    mqtt_pub_freq();
    mqtt_pub_power();
    mqtt_pub_band_b();
    mqtt_pub_freq_b();
    mqtt_pub_split();
    for (int r = 1; r <= NUM_RELAYS; r++) { mqtt_pub_relay(r); mqtt_pub_mode(r); }
    mqtt_pub_state();
}

/* ── MQTT callbacks (network thread — no I2C, no relay-state mutation) ──── */

static void on_connect(struct mosquitto *m, void *u, int rc)
{
    (void)u;
    if (rc != 0) { syslog(LOG_WARNING, "MQTT connect failed rc=%d", rc); return; }
    char t[96];
    snprintf(t, sizeof t, "%s/cmd/#", g_mqtt_prefix);
    mosquitto_subscribe(m, NULL, t, 0);
    g_mqtt_need_republish = 1;     /* RT thread publishes online + full state */
    syslog(LOG_INFO, "MQTT connected; subscribed %s", t);
}

static void on_message(struct mosquitto *m, void *u, const struct mosquitto_message *msg)
{
    (void)m; (void)u;
    if (!msg->topic) return;

    char pfx[96];
    int pl = snprintf(pfx, sizeof pfx, "%s/cmd/", g_mqtt_prefix);
    if (strncmp(msg->topic, pfx, (size_t)pl) != 0) return;
    const char *sub = msg->topic + pl;

    char payload[16] = { 0 };
    int n = msg->payloadlen < (int)sizeof payload - 1 ? msg->payloadlen : (int)sizeof payload - 1;
    if (n > 0) memcpy(payload, msg->payload, n);

    cmd_t c;
    if (!strncmp(sub, "relay/", 6)) {
        int relay = atoi(sub + 6);
        if (relay < 1 || relay > NUM_RELAYS) return;
        c.relay = relay;
        if      (!strcasecmp(payload, "close")) c.kind = CMD_RELAY_CLOSE;
        else if (!strcasecmp(payload, "open"))  c.kind = CMD_RELAY_OPEN;
        else if (!strcasecmp(payload, "auto"))  c.kind = CMD_RELAY_AUTO;
        else return;
    } else if (!strcasecmp(sub, "mode")) {
        c.relay = 0;
        if      (!strcasecmp(payload, "manual")) c.kind = CMD_MODE_MANUAL;
        else if (!strcasecmp(payload, "auto"))   c.kind = CMD_MODE_AUTO;
        else return;
    } else {
        return;
    }

    pthread_mutex_lock(&cmd_mtx);
    int next = (cmd_head + 1) % CMD_QLEN;
    int full = (next == cmd_tail);
    if (!full) { cmd_q[cmd_head] = c; cmd_head = next; }
    pthread_mutex_unlock(&cmd_mtx);
    if (full) syslog(LOG_WARNING, "MQTT command queue full — command dropped");
}

static int mqtt_init(void)
{
    mosquitto_lib_init();
    g_mosq = mosquitto_new("ic905-relay", true, NULL);
    if (!g_mosq) { syslog(LOG_ERR, "mosquitto_new failed"); mosquitto_lib_cleanup(); return -1; }

    if (g_mqtt_user[0])
        mosquitto_username_pw_set(g_mosq, g_mqtt_user, g_mqtt_pass[0] ? g_mqtt_pass : NULL);

    char st[96];
    snprintf(st, sizeof st, "%s/status", g_mqtt_prefix);
    mosquitto_will_set(g_mosq, st, 7, "offline", 0, true);

    mosquitto_connect_callback_set(g_mosq, on_connect);
    mosquitto_message_callback_set(g_mosq, on_message);
    mosquitto_reconnect_delay_set(g_mosq, 1, 30, true);

    int rc = mosquitto_connect_async(g_mosq, g_mqtt_broker, g_mqtt_port, 30);
    if (rc != MOSQ_ERR_SUCCESS)
        syslog(LOG_WARNING, "MQTT connect_async: %s (will retry)", mosquitto_strerror(rc));

    if (mosquitto_loop_start(g_mosq) != MOSQ_ERR_SUCCESS) {
        syslog(LOG_ERR, "mosquitto_loop_start failed");
        mosquitto_destroy(g_mosq); g_mosq = NULL; mosquitto_lib_cleanup();
        return -1;
    }
    syslog(LOG_INFO, "MQTT enabled: %s:%d prefix '%s'%s",
           g_mqtt_broker, g_mqtt_port, g_mqtt_prefix, g_mqtt_user[0] ? " (auth)" : "");
    return 0;
}

static void mqtt_stop(void)
{
    if (!g_mosq) return;
    char st[96];
    snprintf(st, sizeof st, "%s/status", g_mqtt_prefix);
    mosquitto_publish(g_mosq, NULL, st, 7, "offline", 0, true);
    mosquitto_disconnect(g_mosq);
    mosquitto_loop_stop(g_mosq, false);
    mosquitto_destroy(g_mosq);
    mosquitto_lib_cleanup();
    g_mosq = NULL;
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    openlog("ic905-relay", LOG_PID | LOG_CONS | LOG_PERROR, LOG_DAEMON);
    syslog(LOG_INFO, "IC-905 relay sequencer v%s starting", IC905_VERSION);

    setup_signals();

    /* 0. Load band → relay sequencing rules (+ MQTT settings) */
    load_config(CONFIG_PATH);

    if (g_mqtt_enable && mqtt_init() < 0)
        syslog(LOG_WARNING, "MQTT init failed — continuing without monitoring");

    /* 1. GPIO reset PCA9538A boards in use */
    if (gpio_reset_boards() < 0) {
        syslog(LOG_ERR, "GPIO reset failed, exiting");
        closelog();
        return 1;
    }

    /* 2. Initialize I2C and configure PCA9538A registers */
    if (i2c_init() < 0) {
        syslog(LOG_ERR, "I2C init failed, exiting");
        closelog();
        return 1;
    }

    /* 3. Open packet capture */
    pcap_t *handle = setup_pcap(IFACE);
    if (!handle) {
        syslog(LOG_ERR, "pcap setup failed, exiting");
        open_all_relays();
        close(g_i2c_fd);
        closelog();
        return 1;
    }
    g_pcap = handle;

    int fd = pcap_get_selectable_fd(handle);
    if (fd < 0)
        syslog(LOG_WARNING, "no selectable fd — using fallback timing");
    syslog(LOG_INFO, "Entering capture loop");

    /* 4. Main loop: multiplex packet arrival and timed relay events */
    while (g_running) {
        if (fd >= 0) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
            int t = next_timeout_ms();
            /* Wake at least every 50 ms (once we have a band, or MQTT is on) so
               MQTT stays responsive even with no packets. A sooner scheduled
               relay event still wins. */
            if ((g_mqtt_enable || g_have_band) && (t < 0 || t > 50)) t = 50;
            int pr = poll(&pfd, 1, t);
            if (pr < 0) {
                if (errno == EINTR) continue;
                syslog(LOG_ERR, "poll: %s", strerror(errno));
                break;
            }
            if (pr > 0 && (pfd.revents & POLLIN)) {
                int ret = pcap_dispatch(handle, 64, packet_handler, NULL);
                if (ret == PCAP_ERROR) {
                    syslog(LOG_ERR, "pcap_dispatch: %s", pcap_geterr(handle));
                    break;
                }
            }
        } else {
            int ret = pcap_dispatch(handle, 16, packet_handler, NULL);
            if (ret == PCAP_ERROR) {
                syslog(LOG_ERR, "pcap_dispatch: %s", pcap_geterr(handle));
                break;
            }
            int t = next_timeout_ms();
            usleep((t < 0 || t > 2) ? 2000 : (unsigned)t * 1000);
        }
        fire_due_events();
        cmd_drain();                       /* apply any queued MQTT commands */
        apply_state();                     /* publish/act on any pending state change */
        if (g_mqtt_need_republish) { g_mqtt_need_republish = 0; mqtt_republish_all(); }
    }

    /* 5. Cleanup */
    syslog(LOG_INFO, "Shutting down, opening all relays");
    open_all_relays();
    mqtt_stop();
    pcap_close(handle);
    close(g_i2c_fd);
    syslog(LOG_INFO, "Shutdown complete");
    closelog();
    return 0;
}
