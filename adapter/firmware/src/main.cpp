// A1 hotend sensor -> EBB42 adapter
//
// Reads the three analog Hall outputs of the Bambu Lab A1 hotend sensor board
// (MT9105, ratiometric, powered at 3.3 V) and turns each into a clean digital
// line for the EBB42 Endstop connector, using a window comparator with
// programmable thresholds (hysteresis), debounce and output polarity.
//
// Outputs are open-drain. The EBB42 has 10k pull-ups to its own 3.3 V on every
// Endstop input, so "released" reads HIGH there. A released line is also what
// the EBB42 sees when the adapter is unpowered, resetting or in fault, so the
// defaults make HIGH the state that pauses a print (no filament / tangle).
//
// No parts on the inputs: the A1 sensor board already has an RC filter on each
// MT9105 output. A missing sensor is detected by briefly enabling the internal
// pull-up (~45k) on one input at a time: a connected sensor holds the pin near
// its real level, an open input jumps to ~3.3 V.
//
// Configuration is done over the USB serial port (115200, any terminal) and
// stored in NVS. Type `help`.

#include <Arduino.h>
#include <Preferences.h>
#include <driver/gpio.h>

// ---------------------------------------------------------------- pins ----
// ADC1 pins only (ADC2 on the C3 is unreliable). GPIO2 is a strapping pin.
// FPC pin 3 = cutter -> GPIO0, pin 4 = presence -> GPIO1, pin 6 = tangle -> GPIO3
static const uint8_t ADC_PIN[3] = {1, 0, 3};   // presence, cutter, tangle
// To EBB42 Endstop connector (P10): GPIO5 -> pin 3 (PB6), GPIO6 -> pin 2 (PB5),
// GPIO7 -> pin 1 (PB7)
static const uint8_t OUT_PIN[3] = {5, 6, 7};
// Common-ANODE RGB LED plugged straight across GPIO9/10/20/21: its legs
// G A R B line up with those four header pins (resistors in the G, R, B legs).
// The anode sits on GPIO10, driven HIGH after boot; a colour lights when its
// pin is LOW, so the PWM is inverted.
// GPIO9 (green) is the BOOT strapping pin and must read HIGH at reset. At
// reset GPIO10 floats, so no current can flow into GPIO9 through the green LED
// and the pin keeps its pull-up -> normal boot. GPIO9 also has the BOOT button
// to GND, so its drive is limited (~10 mA) in case the button is pressed while
// the pin drives HIGH (green off).
static const uint8_t LED_G = 9, LED_A = 10, LED_R = 20, LED_B = 21;
static const uint8_t LED_ONBOARD = 8;                     // blue, active low

enum { CH_PRESENCE = 0, CH_CUTTER, CH_TANGLE, CH_COUNT };
static const char *CH_NAME[CH_COUNT] = {"presence", "cutter", "tangle"};
static const char *CH_ACTIVE_NAME[CH_COUNT] = {"FILAMENT", "PRESSED", "TANGLE"};
static const char *CH_IDLE_NAME[CH_COUNT] = {"empty", "rest", "ok"};

// --------------------------------------------------------------- config ---
struct ChannelCfg {
  // Thresholds in mV. The direction follows from their order:
  //   on < off : signal FALLS into the active state (active when v <= on,
  //              back to idle when v >= off)
  //   on > off : signal RISES into the active state (active when v >= on,
  //              back to idle when v <= off)
  // The gap between them is the hysteresis.
  uint16_t on_mv;
  uint16_t off_mv;
  uint16_t debounce_ms;   // a new state must hold this long before it is output
  uint8_t active_high;    // line level while active: 1 = released (HIGH), 0 = LOW
};

struct Config {
  uint32_t magic;
  ChannelCfg ch[CH_COUNT];
  uint16_t fault_mv;      // pull-up probe reading above this = sensor missing
  uint8_t led_brightness; // 0..255
  // Tension indicator: the LED starts flashing yellow when the tangle reading
  // passes flash_mv, and the rate rises linearly from flash_min_hz to
  // flash_max_hz as the reading approaches the tangle `on` threshold.
  uint16_t flash_mv;
  uint8_t flash_min_hz;
  uint8_t flash_max_hz;
};

static const uint32_t CFG_MAGIC = 0xA1EBB403;  // bump when Config changes

// Defaults from the bench measurements at 3.3 V (see readme.md):
//   presence : no filament 1.10 V, inserted 0.03 V      -> falls when active
//   cutter   : rest 1.70 V, fully pressed 0.30 V        -> falls when active
//   tangle   : no tangle 0.10 V, fully compressed 2.2 V -> rises when active
// Tangle trip point is a guess: tune it on the machine.
static const Config CFG_DEFAULT = {
    CFG_MAGIC,
    {
        {450, 650, 20, 0},    // presence: filament present -> line LOW
        {850, 1150, 10, 1},   // cutter: lever pressed -> line HIGH
        {1500, 1100, 50, 1},  // tangle: tangle -> line HIGH
    },
    2700,
    96,
    400,  // tangle idles at ~100 mV: start flashing at 400 mV
    1,
    10,
};

static Config cfg;
static Preferences prefs;

// ---------------------------------------------------------------- state ---
struct ChannelState {
  uint16_t mv;            // latest averaged reading
  uint16_t min_mv, max_mv;// peak tracker for the `peak` command
  bool active;            // debounced, comparator output
  bool candidate;         // raw comparator output waiting for debounce
  uint32_t candidate_since;
  bool fault;
  uint16_t probe_mv;      // last pull-up probe reading
  uint8_t probe_streak;   // consecutive probes disagreeing with `fault`
  bool line_released;     // what we drive: true = released (EBB pull-up -> HIGH)
};
static ChannelState st[CH_COUNT];

static const uint32_t SAMPLE_PERIOD_MS = 2;
static const uint8_t OVERSAMPLE = 4;
static const uint32_t PROBE_EVERY = 33;   // samples between probes (~66 ms, one channel each)
static const uint8_t PROBE_CONFIRM = 2;   // consecutive probes needed to change fault state

static uint32_t stream_period_ms = 0;
static uint32_t last_stream_ms = 0;

// learn: captured idle / active readings per channel
static const uint16_t NOT_CAPTURED = 0xFFFF;
static uint16_t learn_idle[CH_COUNT] = {NOT_CAPTURED, NOT_CAPTURED, NOT_CAPTURED};
static uint16_t learn_active[CH_COUNT] = {NOT_CAPTURED, NOT_CAPTURED, NOT_CAPTURED};

// ------------------------------------------------------------- helpers ----
static bool fallsWhenActive(const ChannelCfg &c) { return c.on_mv < c.off_mv; }

static uint16_t readMv(uint8_t pin) {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < OVERSAMPLE; i++) sum += analogReadMilliVolts(pin);
  return sum / OVERSAMPLE;
}

// Window comparator with hysteresis.
static bool compare(const ChannelCfg &c, uint16_t mv, bool current) {
  if (fallsWhenActive(c)) {
    if (mv <= c.on_mv) return true;
    if (mv >= c.off_mv) return false;
  } else {
    if (mv >= c.on_mv) return true;
    if (mv <= c.off_mv) return false;
  }
  return current;  // inside the hysteresis band: keep state
}

static void writeOutput(uint8_t ch) {
  bool level_high;
  if (st[ch].fault) {
    level_high = true;  // release: same as adapter unpowered
  } else {
    level_high = st[ch].active ? cfg.ch[ch].active_high : !cfg.ch[ch].active_high;
  }
  st[ch].line_released = level_high;
  digitalWrite(OUT_PIN[ch], level_high ? HIGH : LOW);  // open drain: HIGH = released
}

static bool validConfig(const Config &c) {
  if (c.magic != CFG_MAGIC) return false;
  for (int i = 0; i < CH_COUNT; i++) {
    if (c.ch[i].on_mv == c.ch[i].off_mv) return false;
    if (c.ch[i].on_mv > 3300 || c.ch[i].off_mv > 3300) return false;
  }
  if (c.flash_min_hz < 1 || c.flash_max_hz < c.flash_min_hz || c.flash_max_hz > 25) return false;
  return true;
}

static void loadConfig() {
  cfg = CFG_DEFAULT;
  prefs.begin("a1ebb", true);
  if (prefs.getBytesLength("cfg") == sizeof(Config)) {
    Config tmp;
    prefs.getBytes("cfg", &tmp, sizeof(tmp));
    if (validConfig(tmp)) cfg = tmp;
  }
  prefs.end();
}

static bool saveConfig() {
  prefs.begin("a1ebb", false);
  size_t n = prefs.putBytes("cfg", &cfg, sizeof(cfg));
  prefs.end();
  return n == sizeof(cfg);
}

// ------------------------------------------------------------------ LED ---
// Common anode: duty 255 = pin always HIGH = colour off.
static void setRgb(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t k = cfg.led_brightness;
  analogWrite(LED_R, 255 - r * k / 255);
  analogWrite(LED_G, 255 - g * k / 255);
  analogWrite(LED_B, 255 - b * k / 255);
}

// Yellow is red + green; the green share sets the hue. Tune it together with
// the series resistors if yellow looks orange or lime.
static const uint8_t YELLOW_G = 180;

// Tension as 0..1000 between flash_mv (0) and the tangle `on` threshold (1000).
// Negative when below flash_mv. Works for either signal direction.
static int32_t tensionPermille() {
  int32_t start = cfg.flash_mv, end = cfg.ch[CH_TANGLE].on_mv;
  if (start == end) return -1;
  int32_t p = ((int32_t)st[CH_TANGLE].mv - start) * 1000 / (end - start);
  return p > 1000 ? 1000 : p;
}

// Priority: fault > cutter > no filament > tangle triggered > tension > all good
static void updateLeds(uint32_t now) {
  static uint32_t last_ms = now;
  static uint32_t phase = 0;  // flash phase, 1000 units per cycle
  uint32_t dt = now - last_ms;
  last_ms = now;

  bool any_fault = st[0].fault || st[1].fault || st[2].fault;
  int32_t tension = tensionPermille();

  if (any_fault) {
    bool on = (now / 125) & 1;                      // magenta 4 Hz: sensor fault
    setRgb(on ? 255 : 0, 0, on ? 255 : 0);
  } else if (st[CH_CUTTER].active) {
    setRgb(0, 0, 255);                              // blue: cutter triggered
  } else if (!st[CH_PRESENCE].active) {
    setRgb(255, 0, 0);                              // red: no filament
  } else if (st[CH_TANGLE].active) {
    setRgb(255, YELLOW_G, 0);                       // solid yellow: PB7 triggered
  } else if (tension >= 0) {
    // flashing yellow, rate rising with tension (accumulated phase so the
    // rate can change smoothly without the blink jumping)
    uint32_t hz_x1000 = cfg.flash_min_hz * 1000 +
                        (uint32_t)(cfg.flash_max_hz - cfg.flash_min_hz) * tension;
    phase = (phase + hz_x1000 * dt / 1000) % 1000;
    bool on = phase < 500;
    setRgb(on ? 255 : 0, on ? YELLOW_G : 0, 0);
  } else {
    phase = 0;                                      // next flash starts "on"
    setRgb(0, 255, 0);                              // green: all good
  }
  // onboard LED: short heartbeat blink once per second
  digitalWrite(LED_ONBOARD, (now % 1000) < 50 ? LOW : HIGH);
}

// ------------------------------------------------------------- sampling ---
// Sensor-presence probe. With the internal pull-up on, a connected MT9105 still
// holds the pin close to its output (it drives through the board's series R),
// while an open input rises to ~3.3 V within microseconds. The pull-up is on for
// ~30 us, too short to disturb the board's filter capacitor.
static void probeChannel(uint8_t ch, bool init) {
  ChannelState &s = st[ch];
  gpio_num_t pin = (gpio_num_t)ADC_PIN[ch];
  gpio_pullup_en(pin);
  delayMicroseconds(20);
  s.probe_mv = analogReadMilliVolts(ADC_PIN[ch]);
  gpio_pullup_dis(pin);

  bool open = s.probe_mv >= cfg.fault_mv;
  if (init) {
    s.fault = open;
    s.probe_streak = 0;
  } else if (open != s.fault) {
    if (++s.probe_streak >= PROBE_CONFIRM) { s.fault = open; s.probe_streak = 0; }
  } else {
    s.probe_streak = 0;
  }
}

static void sampleAll(uint32_t now, bool init) {
  static uint32_t sample_count = 0;
  static uint8_t probe_ch = 0;
  if (init) {
    for (uint8_t ch = 0; ch < CH_COUNT; ch++) probeChannel(ch, true);
  } else if (++sample_count >= PROBE_EVERY) {
    sample_count = 0;
    probeChannel(probe_ch, false);
    probe_ch = (probe_ch + 1) % CH_COUNT;
  }

  for (uint8_t ch = 0; ch < CH_COUNT; ch++) {
    ChannelState &s = st[ch];
    s.mv = readMv(ADC_PIN[ch]);
    if (s.mv < s.min_mv) s.min_mv = s.mv;
    if (s.mv > s.max_mv) s.max_mv = s.mv;

    bool raw = compare(cfg.ch[ch], s.mv, s.candidate);
    if (init) {
      s.active = s.candidate = raw;
      s.candidate_since = now;
    } else {
      if (raw != s.candidate) { s.candidate = raw; s.candidate_since = now; }
      if (s.candidate != s.active && now - s.candidate_since >= cfg.ch[ch].debounce_ms)
        s.active = s.candidate;
    }
    writeOutput(ch);
  }
}

// ------------------------------------------------------------------ CLI ---
static void printChannelCfg(uint8_t ch) {
  const ChannelCfg &c = cfg.ch[ch];
  Serial.printf("  %-8s on=%4u off=%4u mV (%s when active, hyst %u mV) debounce=%u ms, "
                "line %s when %s\n",
                CH_NAME[ch], c.on_mv, c.off_mv, fallsWhenActive(c) ? "falls" : "rises",
                (unsigned)abs((int)c.on_mv - (int)c.off_mv), c.debounce_ms,
                c.active_high ? "HIGH" : "LOW", CH_ACTIVE_NAME[ch]);
}

static void printStatus() {
  Serial.println("config:");
  for (uint8_t ch = 0; ch < CH_COUNT; ch++) printChannelCfg(ch);
  Serial.printf("  fault above %u mV, LED brightness %u\n", cfg.fault_mv, cfg.led_brightness);
  Serial.printf("  tension flash from %u mV, %u..%u Hz up to tangle on=%u mV\n", cfg.flash_mv,
                cfg.flash_min_hz, cfg.flash_max_hz, cfg.ch[CH_TANGLE].on_mv);
  Serial.println("now:");
  for (uint8_t ch = 0; ch < CH_COUNT; ch++) {
    const ChannelState &s = st[ch];
    Serial.printf("  %-8s %4u mV  %-8s line=%s  probe=%4u mV%s\n", CH_NAME[ch], s.mv,
                  s.active ? CH_ACTIVE_NAME[ch] : CH_IDLE_NAME[ch],
                  s.line_released ? "HIGH(released)" : "LOW", s.probe_mv,
                  s.fault ? "  FAULT (sensor not connected?)" : "");
  }
}

static void printStreamLine() {
  for (uint8_t ch = 0; ch < CH_COUNT; ch++) {
    const ChannelState &s = st[ch];
    Serial.printf("%s=%4u %c%s  ", CH_NAME[ch], s.mv, s.active ? '1' : '0', s.fault ? "F" : "");
  }
  Serial.println();
}

static void printHelp() {
  Serial.println(
      "commands (channel = presence|cutter|tangle or 0|1|2, prefixes ok):\n"
      "  status                     config, live readings, line levels\n"
      "  stream <ms>                print readings every <ms> (0 = stop)\n"
      "  peak                       min/max mV per channel since last `peak`, then reset\n"
      "  set <ch> on <mV>           threshold that enters the active state\n"
      "  set <ch> off <mV>          threshold that returns to idle (hysteresis)\n"
      "  set <ch> debounce <ms>\n"
      "  set <ch> level high|low    line level while active\n"
      "  learn <ch> idle|active     capture the current reading (hold the state ~0.5 s)\n"
      "  learn <ch> apply           thresholds at 35%/65% between the captured points\n"
      "  fault <mV>                 pull-up probe above this flags a missing sensor\n"
      "  bright <0-255>             RGB LED brightness\n"
      "  flash <mV> [min_hz max_hz] tangle level where yellow flashing starts, and its rate range\n"
      "  save | load | defaults     NVS storage (changes are live but not kept until save)");
}

static int parseChannel(const String &s) {
  if (s.length() == 0) return -1;
  if (s.length() == 1 && isDigit(s[0])) {
    int n = s.toInt();
    return n < CH_COUNT ? n : -1;
  }
  for (int i = 0; i < CH_COUNT; i++)
    if (String(CH_NAME[i]).startsWith(s)) return i;
  return -1;
}

// Average a channel over ~500 ms for the learn command.
static uint16_t captureMv(uint8_t ch) {
  uint32_t sum = 0;
  const int n = 100;
  for (int i = 0; i < n; i++) {
    sum += analogReadMilliVolts(ADC_PIN[ch]);
    delay(5);
  }
  return sum / n;
}

static void handleCommand(String line) {
  line.trim();
  line.toLowerCase();
  if (line.length() == 0) return;

  // split into up to 4 tokens
  String tok[4];
  int nt = 0, pos = 0;
  while (nt < 4 && pos < (int)line.length()) {
    int sp = line.indexOf(' ', pos);
    if (sp < 0) sp = line.length();
    if (sp > pos) tok[nt++] = line.substring(pos, sp);
    pos = sp + 1;
  }
  const String &cmd = tok[0];

  if (cmd == "help" || cmd == "?") {
    printHelp();
  } else if (cmd == "status" || cmd == "s") {
    printStatus();
  } else if (cmd == "stream") {
    stream_period_ms = tok[1].toInt();
    if (stream_period_ms && stream_period_ms < 20) stream_period_ms = 20;
  } else if (cmd == "peak") {
    for (uint8_t ch = 0; ch < CH_COUNT; ch++) {
      Serial.printf("  %-8s min=%4u max=%4u mV\n", CH_NAME[ch], st[ch].min_mv, st[ch].max_mv);
      st[ch].min_mv = 0xFFFF;
      st[ch].max_mv = 0;
    }
  } else if (cmd == "set" && nt == 4) {
    int ch = parseChannel(tok[1]);
    if (ch < 0) { Serial.println("bad channel"); return; }
    ChannelCfg c = cfg.ch[ch];
    long v = tok[3].toInt();
    if (tok[2] == "on") c.on_mv = constrain(v, 0, 3300);
    else if (tok[2] == "off") c.off_mv = constrain(v, 0, 3300);
    else if (tok[2] == "debounce") c.debounce_ms = constrain(v, 0, 5000);
    else if (tok[2] == "level") {
      if (tok[3] == "high") c.active_high = 1;
      else if (tok[3] == "low") c.active_high = 0;
      else { Serial.println("level high|low"); return; }
    } else { Serial.println("unknown field"); return; }
    if (c.on_mv == c.off_mv) { Serial.println("on and off must differ"); return; }
    cfg.ch[ch] = c;
    printChannelCfg(ch);
  } else if (cmd == "learn" && nt == 3) {
    int ch = parseChannel(tok[1]);
    if (ch < 0) { Serial.println("bad channel"); return; }
    if (tok[2] == "idle" || tok[2] == "active") {
      uint16_t mv = captureMv(ch);
      (tok[2] == "idle" ? learn_idle : learn_active)[ch] = mv;
      Serial.printf("  %s %s = %u mV\n", CH_NAME[ch], tok[2].c_str(), mv);
    } else if (tok[2] == "apply") {
      if (learn_idle[ch] == NOT_CAPTURED || learn_active[ch] == NOT_CAPTURED) { Serial.println("capture idle and active first"); return; }
      int idle = learn_idle[ch], act = learn_active[ch];
      if (abs(act - idle) < 200) { Serial.println("idle and active less than 200 mV apart, refusing"); return; }
      cfg.ch[ch].on_mv = idle + (act - idle) * 65 / 100;
      cfg.ch[ch].off_mv = idle + (act - idle) * 35 / 100;
      printChannelCfg(ch);
      Serial.println("  (not saved yet: `save` to keep)");
    } else {
      Serial.println("learn <ch> idle|active|apply");
    }
  } else if (cmd == "fault" && nt == 2) {
    cfg.fault_mv = constrain(tok[1].toInt(), 500, 3300);
    Serial.printf("  fault above %u mV\n", cfg.fault_mv);
  } else if (cmd == "flash" && (nt == 2 || nt == 4)) {
    uint16_t mv = constrain(tok[1].toInt(), 0, 3300);
    uint8_t lo = cfg.flash_min_hz, hi = cfg.flash_max_hz;
    if (nt == 4) { lo = constrain(tok[2].toInt(), 1, 25); hi = constrain(tok[3].toInt(), 1, 25); }
    if (hi < lo) { Serial.println("max_hz must be >= min_hz"); return; }
    if (mv == cfg.ch[CH_TANGLE].on_mv) { Serial.println("must differ from tangle on threshold"); return; }
    cfg.flash_mv = mv; cfg.flash_min_hz = lo; cfg.flash_max_hz = hi;
    Serial.printf("  tension flash from %u mV, %u..%u Hz\n", mv, lo, hi);
  } else if (cmd == "bright" && nt == 2) {
    cfg.led_brightness = constrain(tok[1].toInt(), 0, 255);
  } else if (cmd == "save") {
    Serial.println(saveConfig() ? "saved" : "save FAILED");
  } else if (cmd == "load") {
    loadConfig();
    Serial.println("loaded");
  } else if (cmd == "defaults") {
    cfg = CFG_DEFAULT;
    Serial.println("defaults restored (not saved yet)");
  } else {
    Serial.println("? (type help)");
  }
}

static void pollSerial() {
  static String buf;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (buf.length()) handleCommand(buf);
      buf = "";
    } else if (buf.length() < 64) {
      buf += c;
    }
  }
}

// ------------------------------------------------------------ arduino -----
void setup() {
  // Keep lines released until the first real reading is in.
  for (uint8_t ch = 0; ch < CH_COUNT; ch++) {
    pinMode(OUT_PIN[ch], OUTPUT_OPEN_DRAIN);
    digitalWrite(OUT_PIN[ch], HIGH);
  }
  pinMode(LED_ONBOARD, OUTPUT);
  digitalWrite(LED_ONBOARD, HIGH);

  Serial.begin(115200);
  loadConfig();

  analogReadResolution(12);
  for (uint8_t ch = 0; ch < CH_COUNT; ch++) {
    analogSetPinAttenuation(ADC_PIN[ch], ADC_11db);  // ~0..2.5 V linear on the C3
    st[ch].min_mv = 0xFFFF;
    st[ch].max_mv = 0;
  }

  sampleAll(millis(), true);

  // RGB LED: attach PWM with every colour off (pins HIGH) before powering the
  // common anode, so it never flashes at boot.
  setRgb(0, 0, 0);
  gpio_set_drive_capability((gpio_num_t)LED_G, GPIO_DRIVE_CAP_1);  // see BOOT note above
  pinMode(LED_A, OUTPUT);
  gpio_set_drive_capability((gpio_num_t)LED_A, GPIO_DRIVE_CAP_3);  // sources all 3 colours
  digitalWrite(LED_A, HIGH);
  updateLeds(millis());
}

void loop() {
  static uint32_t last_sample = 0;
  uint32_t now = millis();

  if (now - last_sample >= SAMPLE_PERIOD_MS) {
    last_sample = now;
    sampleAll(now, false);
    updateLeds(now);
  }

  if (stream_period_ms && now - last_stream_ms >= stream_period_ms) {
    last_stream_ms = now;
    printStreamLine();
  }

  pollSerial();
}
