#include <EEPROM.h>
#include <WiFi.h>
#include <time.h>
#include <ESPmDNS.h>
#define ARDUINOJSON_USE_DOUBLE 1
#define ARDUINOJSON_POSITIVE_EXPONENTIATION_THRESHOLD 1e9
#define ARDUINOJSON_NEGATIVE_EXPONENTIATION_THRESHOLD 1e-9
#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>
#include "freertos/task.h"
#include "esp_task_wdt.h"

#define NRPUMPS 5
const int pins[NRPUMPS] = {12, 27, 33, 14, 22};
#define MICRO 1000000

using namespace websockets;

const char* server_host PROGMEM = "bcws.axw.se";
const uint16_t server_port = 80;
const char* ntpServer PROGMEM = "pool.ntp.org";
const long  gmtOffset_sec = 3600;
const int   daylightOffset_sec = 3600;
const size_t capacity = 1024;

WebsocketsClient client;
WebsocketsServer server;
WebsocketsClient server_client;
struct tm boot_time;
bool have_time = false;

void sendJson(JsonObject obj);
void get_time(JsonObject res);
void main_task(void* arg);
QueueHandle_t send_queue;
struct pump_message {
  enum state_t {PUMP_START, PUMP_STOP, PUMP_DISABLED} message;
  int pump;
  double us;
  double ml;
  double ml_dosed;
  int disabled_for;
};


void print_mem() {
  char ptrTaskList[512];
  vTaskList(ptrTaskList);
  Serial.println(F("Task Name\tStatus\tPrio\tHWM\tTask\tAffinity"));
  Serial.print(ptrTaskList);
}

struct PumpStorage {
  double ml_per_us = 0;
  int trigger_min = 0;
  double schedule[24] = {};
};

struct Storage {
  unsigned long checksum = 0;
  PumpStorage p_data[NRPUMPS];

  unsigned long calc_checksum() {
    unsigned long sum = 0;
    char *bytes = (char *)&p_data;
    for(int i = 0; i < sizeof(p_data); i++) {
      sum += bytes[i];
    }
    return sum;
  }
} storage;

struct Pump {
  int pin;
  int pump;
  enum state_t {IDLE, RUNNING} state;
  unsigned long run_start = 0;
  unsigned long run_for = 0;
  int last_triggered = -1;
  int disabled_for = 0;
  double ml_dosed = 0;
  bool nocount = false;
  PumpStorage *data;

  void turn_off() {
    digitalWrite(pin, LOW);
    unsigned long running_for = micros() - run_start;
    if(!nocount) {
      ml_dosed += running_for * data->ml_per_us;
    }
    nocount = false;
    run_for = 0;
    state = IDLE;
  }
  void add_run_info(unsigned long us, JsonObject nfo) {
    nfo[F("msg")] = F("pump_started");
    nfo[F("pump")] = pump;
    nfo[F("ml")] =  us * data->ml_per_us;
    nfo[F("us")] = us;
    nfo[F("dosed")] = ml_dosed;
  }

  void add_stop_info(JsonObject res) {
    res[F("msg")] = F("pump_stopped");
    res[F("pump")] = pump;
    res[F("dosed")] = ml_dosed;
  }

  void turn_on(unsigned long us) {
    digitalWrite(pin, HIGH);
    run_for = us;
    run_start = micros();
    state = RUNNING;
  }

  void run_request(unsigned long us, bool nc, JsonObject res) {
    if(state == IDLE) {
      add_run_info(us, res);
      nocount = nc;
      turn_on(us);
    } else {
      res[F("msg")] = F("error");
      res[F("error")] = F("Pump is running");
    }
  }

  void stop(JsonObject res) {
    turn_off();
    add_stop_info(res);
  }

  void update(tm &time) {
    if(state == RUNNING) {
      unsigned long running_for = micros() - run_start;
      if(running_for >= run_for) {
        turn_off();
        struct pump_message msg;
        msg.message = pump_message::PUMP_STOP;
        msg.pump = pump;
        msg.ml_dosed = ml_dosed;
        xQueueSend(send_queue, &msg, portMAX_DELAY);
      }
    } else if (state == IDLE) {
      if(time.tm_hour != last_triggered && time.tm_min == data->trigger_min) {
        last_triggered = time.tm_hour;
        auto ml = data->schedule[time.tm_hour];
        auto ml_per_us = data->ml_per_us;
        if(disabled_for > 0) {
          disabled_for--;
          struct pump_message msg;
          msg.message = pump_message::PUMP_DISABLED;
          msg.pump = pump;
          msg.disabled_for = disabled_for;
          xQueueSend(send_queue, &msg, portMAX_DELAY);
          return;
        }
        if(ml_per_us == 0 || ml == 0) {
          return;
        }
        auto us = ml / ml_per_us;
        struct pump_message msg;
        msg.message = pump_message::PUMP_START;
        msg.pump = pump;
        msg.us = us;
        msg.ml = us * ml_per_us;
        msg.ml_dosed = ml_dosed;
        xQueueSend(send_queue, &msg, portMAX_DELAY);
        turn_on(us);
      }
    }
  }
};

Pump pumps[NRPUMPS];


void save() {
  storage.checksum = storage.calc_checksum();
  EEPROM.put(0, storage);
  EEPROM.commit();
  Serial.println(F("Saved"));
}

void load() {
  Storage loaded;
  EEPROM.get(0, loaded);
  if(loaded.calc_checksum() == loaded.checksum) {
    storage = loaded;
    Serial.print(F("Successfully loaded from EEPROM"));
  } else {
    Serial.println(F("Failed to load from EEPROM"));
    save();
  }
}

void sendJson(JsonObject obj) {
  String output;
  serializeJson(obj, output);
  client.send(output);
  server_client.send(output);
}

Pump* pump_by_id(int id) {
  if(id > 0 && id <= NRPUMPS) {
    return &pumps[id-1];
  }
  return nullptr;
}

void run_pump(int id, unsigned long us, double ml, bool nocount, JsonObject res) {
  Pump* pump = pump_by_id(id);
  if(pump != nullptr) {
    auto ml_per_us = pump->data->ml_per_us;
    if(ml > 0) {
      if(ml_per_us == 0) {
        res[F("msg")] = F("error");
        res[F("error")] = F("Pump not calibrated");
        return;
      }
      us = ml / ml_per_us;
    }
    if (us == 0) {
      res[F("msg")] = F("error");
      res[F("error")] = F("Invalid amount");
      return;
    }
    pump->run_request(us, nocount, res);
  } else {
    res[F("msg")] = F("error");
    res[F("error")] = F("Invalid pump");
  }
}

void stop_pump(int id, JsonObject res) {
  Pump* pump = pump_by_id(id);
  if(pump == nullptr) {
    res[F("msg")] = F("error");
    res[F("error")] = F("Invalid pump");
    return;
  }
  pump->stop(res);
}

void set_cal(int id, double ml, double us, JsonObject res) {
  Pump* pump = pump_by_id(id);
  if(pump == nullptr) {
    res[F("msg")] = F("error");
    res[F("error")] = F("Invalid pump");
    return;
  }
  if (!(ml > 0 && us > 0)) {
    res[F("msg")] = F("error");
    res[F("error")] = F("Invalid values");
    return;
  }
  double ml_per_us = ml / us;
  pump->data->ml_per_us = ml_per_us;
  save();
  res[F("msg")] = F("ok");
  res[F("ml_per_us")] = ml_per_us;
}

void timeToString(struct tm* tm, char* out, size_t len) {
  snprintf_P(out, len, PSTR("%04d-%02d-%02d %02d:%02d:%02d"),
           tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
           tm->tm_hour, tm->tm_min, tm->tm_sec);
}

void get_time(JsonObject res) {
  char t[32];
  struct tm tm;
  getLocalTime(&tm);
  timeToString(&tm, t, sizeof(t));
  res[F("time")] = t;
}

void get_boot_time(JsonObject res) {
  char b[32];
  timeToString(&boot_time, b, sizeof(b));
  res[F("boot")] = b;
}

void get_state(JsonObject res) {
  res[F("msg")] = F("ok");
  JsonArray array = res.createNestedArray(F("pumps"));
  for(int i = 0; i < NRPUMPS; i++) {
    JsonObject p = array.createNestedObject();
    p[F("pump")] = i + 1;
    p[F("minute")] = pumps[i].data->trigger_min;
    p[F("ml_per_us")] = pumps[i].data->ml_per_us;
    p[F("dosed")] = pumps[i].ml_dosed;
    p[F("disabled")] = pumps[i].disabled_for;
    if(pumps[i].state == Pump::state_t::RUNNING) {
      unsigned long running_for = micros() - pumps[i].run_start;
      p[F("running")] = running_for;
      p[F("us")] = pumps[i].run_for;
    }
    JsonArray schedule = p.createNestedArray(F("schedule"));
    for(int j = 0; j < 24; j++) {
      schedule.add(pumps[i].data->schedule[j]);
    }
  }
}

void disable(JsonArray pumps, JsonVariant disable, JsonObject res) {
  int dis = disable.as<int>();
  for (JsonVariant pump_id : pumps) {
    if(pump_by_id(pump_id.as<int>()) == nullptr) {
      res[F("msg")] = F("error");
      res[F("error")] = F("Invalid pump");
      return;
    }
  }
  if(!disable.is<int>() || dis < 0) {
    res[F("msg")] = F("error");
    res[F("error")] = F("Invalid number of periods");
    return;
  }
  for (JsonVariant pump_id : pumps) {
    Pump* pump = pump_by_id(pump_id);
    pump->disabled_for = dis;
  }
  res[F("msg")] = F("ok");
}

void set_sched(JsonArray pumps, JsonArray sched, JsonObject res) {
  if(sched.size() != 24) {
    res[F("msg")] = F("error");
    res[F("error")] = F("Invalid schedule size");
    return;
  }
  if(pumps.size() == 0) {
    res[F("msg")] = F("error");
    res[F("error")] = F("No pumps selected");
    return;
  }
  for (JsonVariant pump_id : pumps) {
    if(pump_by_id(pump_id.as<int>()) == nullptr) {
      res[F("msg")] = F("error");
      res[F("error")] = F("Invalid pump");
      return;
    }
  }
  for (JsonVariant value : sched) {
    if(!value.is<double>()) {
      res[F("msg")] = F("error");
      res[F("error")] = F("Invalid schedule entry");
      return;
    }
  }
  for (JsonVariant pump_id : pumps) {
    Pump* pump = pump_by_id(pump_id);
    for(int hr = 0; hr <24; hr++) {
      pump->data->schedule[hr] = sched[hr];
    }
  }
  save();
  res[F("msg")] = F("ok");
}

void reset_dosed(JsonArray pumps, JsonObject res) {
  if(pumps.size() == 0) {
    res[F("msg")] = F("error");
    res[F("error")] = F("No pumps selected");
    return;
  }
  for (JsonVariant pump_id : pumps) {
    if(pump_by_id(pump_id.as<int>()) == nullptr) {
      res[F("msg")] = F("error");
      res[F("error")] = F("Invalid pump");
      return;
    }
  }
  for (JsonVariant pump_id : pumps) {
    Pump* pump = pump_by_id(pump_id);
    pump->ml_dosed = 0;
  }
  res[F("msg")] = F("ok");
}

void set_spread(JsonArray minutes, JsonObject res) {
  if(minutes.size() != NRPUMPS) {
    res[F("msg")] = F("error");
    res[F("error")] = F("Invalid number of minutes");
    return;
  }
  for (int i = 0; i < minutes.size(); i++) {
    int minute = minutes[i].as<int>();
    if(!minutes[i].is<int>() && minute >= 0 && minute < 60) {
      res[F("msg")] = F("error");
      res[F("error")] = F("Invalid minute");
      return;
    }
  }
  for (int i = 0; i < minutes.size(); i++) {
    pumps[i].data->trigger_min = minutes[i];
  }
  save();
  res[F("msg")] = F("ok");
}

void wifi_scan(JsonObject res) {
  int n = WiFi.scanNetworks();
  JsonArray array = res.createNestedArray(F("networks"));
  for (int i = 0; i < n; ++i) {
    JsonObject network = array.createNestedObject();
    network[F("ssid")] = WiFi.SSID(i);
    network[F("rssi")] = WiFi.RSSI(i);
  }
  res[F("msg")] = F("ok");
}

void onJson(JsonObject obj) {
  DynamicJsonDocument doc(4096);
  JsonObject res = doc.to<JsonObject>();
  if (obj.containsKey(F("id"))) {
    res[F("ack")] = obj[F("id")];
  }
  if (          strcmp_P(obj[F("msg")], PSTR("get_time")) == 0) {
    res[F("msg")] = F("ok");
    get_time(res);
  } else if (   strcmp_P(obj[F("msg")], PSTR("run_pump")) == 0) {
    int id           = obj[F("pump")];
    unsigned long us = obj[F("us")];
    double ml        = obj[F("ml")];
    bool nocount     = obj[F("nocount")];
    run_pump(id, us, ml, nocount, res);
  } else if (   strcmp_P(obj[F("msg")], PSTR("stop_pump")) == 0) {
    int id           = obj[F("pump")];
    stop_pump(id, res);
  } else if(    strcmp_P(obj[F("msg")], PSTR("disable")) == 0) {
    JsonArray pumps  = obj[F("pumps")];
    JsonVariant dis  = obj[F("disable")];
    disable(pumps, dis, res);
  } else if(    strcmp_P(obj[F("msg")], PSTR("set_cal")) == 0) {
    int id           = obj[F("pump")];
    double ml        = obj[F("ml")];
    double us        = obj[F("us")];
    set_cal(id, ml, us, res);
  } else if(    strcmp_P(obj[F("msg")], PSTR("set_spread")) == 0) {
    JsonArray minutes  = obj[F("minutes")];
    set_spread(minutes, res);
  } else if(    strcmp_P(obj[F("msg")], PSTR("set_sched")) == 0) {
    JsonArray pumps  = obj[F("pumps")];
    JsonArray sched  = obj[F("schedule")];
    set_sched(pumps, sched, res);
  } else if(    strcmp_P(obj[F("msg")], PSTR("reset_dosed")) == 0) {
    JsonArray pumps  = obj[F("pumps")];
    reset_dosed(pumps, res);
  } else if(    strcmp_P(obj[F("msg")], PSTR("get_state")) == 0) {
    get_time(res);
    get_boot_time(res);
    get_state(res);
  } else if(    strcmp_P(obj[F("msg")], PSTR("divide")) == 0) {
    int a = obj[F("a")];
    int b = obj[F("b")];
    res[F("msg")] = F("divide_res");
    res[F("res")] = a / b;
  } else if(    strcmp_P(obj[F("msg")], PSTR("wifi_scan")) == 0) {
    wifi_scan(res);
  }

  if(res.containsKey(F("msg"))) {
    sendJson(res);
  }
}

void onWsMsg(WebsocketsMessage message) {
  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, message.data());
  JsonObject obj = doc.as<JsonObject>();
  if (error || obj.isNull()) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.c_str());
    Serial.print(F("Got Message: "));
    Serial.println(message.data());
    return;
  }
  onJson(obj);
}

void onWsEvent(WebsocketsEvent event, String data) {
  if (event == WebsocketsEvent::ConnectionOpened) {
    Serial.println(F("Connnection Opened"));
  } else if (event == WebsocketsEvent::ConnectionClosed) {
    Serial.println(F("Connnection Closed"));
  } else if (event == WebsocketsEvent::GotPing) {
    Serial.println(F("Got a Ping!"));
  } else if (event == WebsocketsEvent::GotPong) {
  }
}

bool have_ip = false;
void onWifiEvent(WiFiEvent_t event) {
  if (       event == SYSTEM_EVENT_STA_DISCONNECTED) {
    have_ip = false;
  } else if (event == SYSTEM_EVENT_STA_GOT_IP) {
    have_ip = true;
  }
  digitalWrite(13, !have_ip);
}

void sync_time() {
  configTzTime(PSTR("CET-1CEST,M3.5.0,M10.5.0/3"), ntpServer);
}

static TaskHandle_t main_task_handle = NULL;
static TaskHandle_t pump_task_handle = NULL;


void pump_task(void* arg) {
  struct tm tm;
  TickType_t xLastWakeTime = xTaskGetTickCount();
  for(;;) {
    if(have_time) {
      getLocalTime(&tm);
      // Debug mode
      // tm.tm_hour  = tm.tm_min % 24;
      // tm.tm_min = tm.tm_sec;
      for(int i = 0; i < NRPUMPS; i++) {
        pumps[i].update(tm);
      }
    }
    vTaskDelayUntil(&xLastWakeTime, 1);
  }
  vTaskDelete(NULL);
  pump_task_handle = NULL;
}

byte triggerHour = -1;
void setup() {
  Serial.begin(115200);
  while (!EEPROM.begin(sizeof(Storage))) {
  }
  load();
  for(int i = 0; i < NRPUMPS; i++) {
    pinMode(pins[i], OUTPUT);
    digitalWrite(pins[i], LOW);
    pumps[i].pump = i + 1;
    pumps[i].pin = pins[i];
    pumps[i].data = &storage.p_data[i];
  }
  Serial.println(F("\nHello lexpump"));
  pinMode(13, OUTPUT);
  digitalWrite(13, HIGH);
  WiFi.setHostname(PSTR("lexpump"));
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(PSTR("lexpump"));
  WiFi.onEvent(onWifiEvent);
  WiFi.begin(PSTR("L3-wifi"), PSTR("L3333333"));
  MDNS.begin(PSTR("lexpump"));

  server.listen(80);
  client.onMessage(onWsMsg);
  client.onEvent(onWsEvent);

  send_queue = xQueueCreate(10, sizeof(struct pump_message));

  xTaskCreatePinnedToCore(pump_task, PSTR("pump_task"), 8192, NULL, 1, &pump_task_handle, PRO_CPU_NUM);
  xTaskCreatePinnedToCore(main_task, PSTR("main_task"), 32768, NULL, 1, &main_task_handle, APP_CPU_NUM);
  disableLoopWDT();
  vTaskDelete(NULL);
}

void ping_loop() {
  static unsigned long prev = 0;
  unsigned long now = micros();
  if((now - prev) >= 45 * MICRO) {
    prev = now;
    client.ping();
  }
}

void reconnect_loop() {
  static unsigned long prev = 0;
  unsigned long now = micros();
  if((now - prev) >= 5 * MICRO) {
    prev = now;
    if (have_ip) {
      Serial.println(F("Reconnecting to server"));
      client.connect(server_host, server_port, F("/ws"));
    } else {
      Serial.println(F("Reconnecting to wifi"));
      WiFi.reconnect();
    }
  }
}

void sync_time_loop() {
  static unsigned long prev = 0;
  unsigned long now = micros();
  if((now - prev) > 5 * 60 * MICRO || prev == 0) {
    prev = now;
    sync_time();
  }
}

void mem_info_loop() {
  static unsigned long prev_mem_info = 0;
  unsigned long now = micros();
  if((now - prev_mem_info) > 5 * MICRO) {
    prev_mem_info = now;
    print_mem();
  }
}

void receive_send_queue() {
  struct pump_message msg;
  if (xQueueReceive(send_queue, &msg, 0)) {
    StaticJsonDocument<capacity> nfo;
    if(msg.message == pump_message::PUMP_START) {
      nfo[F("msg")] = F("pump_started");
      nfo[F("pump")] = msg.pump;
      nfo[F("ml")] =  msg.ml;
      nfo[F("us")] = msg.us;
      nfo[F("dosed")] = msg.ml_dosed;
    } else if(msg.message == pump_message::PUMP_STOP) {
      nfo[F("msg")] = F("pump_stopped");
      nfo[F("pump")] = msg.pump;
      nfo[F("dosed")] = msg.ml_dosed;
    } else if(msg.message == pump_message::PUMP_DISABLED) {
      nfo[F("msg")] = F("skipped");
      nfo[F("pump")] = msg.pump;
      nfo[F("disabled")] = msg.disabled_for;
    }
    sendJson(nfo.as<JsonObject>());
  }
}

void loop() {
}


void main_task(void* arg) {
  for(;;) {

  if(have_ip) {
    sync_time_loop();
  }
  if (server.poll()) {
    server_client = server.accept();
    server_client.onMessage(onWsMsg);
    server_client.onEvent(onWsEvent);
  }
  if(server_client.available()) {
    server_client.poll();
  }
  if (client.available()) {
    client.poll();
    ping_loop();
  } else {
    reconnect_loop();
  }
  struct tm tm;
  getLocalTime(&tm);
  if ((tm.tm_year + 1900) < 2000) {
    Serial.println(F("Waiting for time"));
    delay(1000);
    continue;
  } else {
    have_time = true;
  }
  if ((boot_time.tm_year + 1900) < 2000) {
    boot_time = tm;
  }

  receive_send_queue();

  vTaskDelay(1);
  }
}
