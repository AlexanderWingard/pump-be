#include <EEPROM.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#define ARDUINOJSON_USE_DOUBLE 1
#define ARDUINOJSON_POSITIVE_EXPONENTIATION_THRESHOLD 1e9
#define ARDUINOJSON_NEGATIVE_EXPONENTIATION_THRESHOLD 1e-9
#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>
#include <AutoConnect.h>


#define NRPUMPS 5
const int pins[NRPUMPS] = {12, 27, 33, 14, 22};
#define MICRO 1000000

using namespace websockets;

const char* server_host = "bcws.axw.se";
const uint16_t server_port = 80;
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;
const int   daylightOffset_sec = 3600;
const size_t capacity = 1024;

WebServer Server;
AutoConnect Portal(Server);
AutoConnectConfig Config;
WebsocketsClient client;
struct tm boot_time;


void sendJson(JsonObject obj);
void get_time(JsonObject res);

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
  PumpStorage *data;

  void turn_off() {
    digitalWrite(pin, LOW);
    unsigned long running_for = micros() - run_start;
    ml_dosed += running_for * data->ml_per_us;
    run_for = 0;
    state = IDLE;
  }
  void add_run_info(unsigned long us, JsonObject nfo) {
    nfo["msg"] = "pump_started";
    nfo["pump"] = pump;
    nfo["ml"] =  us * data->ml_per_us;
    nfo["us"] = us;
    nfo["dosed"] = ml_dosed;
  }

  void add_stop_info(JsonObject res) {
    res["msg"] = "pump_stopped";
    res["pump"] = pump;
    res["dosed"] = ml_dosed;
  }

  void turn_on(unsigned long us) {
    run_for = us;
    run_start = micros();
    state = RUNNING;
    digitalWrite(pin, HIGH);
  }

  void run_request(unsigned long us, JsonObject res) {
    if(state == IDLE) {
      add_run_info(us, res);
      turn_on(us);
    } else {
      res["msg"] = "error";
      res["error"] = "Pump is running";
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
        StaticJsonDocument<capacity> doc;
        JsonObject res = doc.to<JsonObject>();
        add_stop_info(res);
        sendJson(res);
      }
    } else if (state == IDLE) {
      if(time.tm_hour != last_triggered && time.tm_min == data->trigger_min) {
        last_triggered = time.tm_hour;
        auto ml = data->schedule[time.tm_hour];
        auto ml_per_us = data->ml_per_us;
        if(disabled_for > 0) {
          disabled_for--;
          StaticJsonDocument<capacity> doc;
          JsonObject res = doc.to<JsonObject>();
          res["msg"] = "skipped";
          res["pump"] = pump;
          res["disabled"] = disabled_for;
          sendJson(res);
          return;
        }
        if(ml_per_us == 0 || ml == 0) {
          return;
        }
        auto us = ml / ml_per_us;
        StaticJsonDocument<capacity> doc;
        JsonObject res = doc.to<JsonObject>();
        add_run_info(us, res);
        sendJson(res);
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
  Serial.println("Saved");
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
  char output[2048];
  serializeJson(obj, output);
  client.send(output);
}

Pump* pump_by_id(int id) {
  if(id > 0 && id <= NRPUMPS) {
    return &pumps[id-1];
  }
  return nullptr;
}

void run_pump(int id, unsigned long us, double ml, JsonObject res) {
  Pump* pump = pump_by_id(id);
  if(pump != nullptr) {
    auto ml_per_us = pump->data->ml_per_us;
    if(ml > 0) {
      if(ml_per_us == 0) {
        res["msg"] = "error";
        res["error"] = "Pump not calibrated";
        return;
      }
      us = ml / ml_per_us;
    }
    if (us == 0) {
      res["msg"] = "error";
      res["error"] = "Invalid amount";
      return;
    }
    pump->run_request(us, res);
  } else {
    res["msg"] = "error";
    res["error"] = "Invalid pump";
  }
}

void stop_pump(int id, JsonObject res) {
  Pump* pump = pump_by_id(id);
  if(pump == nullptr) {
    res["msg"] = "error";
    res["error"] = "Invalid pump";
    return;
  }
  pump->stop(res);
}

void set_cal(int id, double ml, double us, JsonObject res) {
  Pump* pump = pump_by_id(id);
  if(pump == nullptr) {
    res["msg"] = "error";
    res["error"] = "Invalid pump";
    return;
  }
  if (!(ml > 0 && us > 0)) {
    res["msg"] = "error";
    res["error"] = "Invalid values";
    return;
  }
  double ml_per_us = ml / us;
  pump->data->ml_per_us = ml_per_us;
  save();
  res["msg"] = "ok";
  res["ml_per_us"] = ml_per_us;
}

void timeToString(struct tm* tm, char* out, size_t len) {
  snprintf(out, len, "%04d-%02d-%02d %02d:%02d:%02d",
           tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
           tm->tm_hour, tm->tm_min, tm->tm_sec);
}

void get_time(JsonObject res) {
  char t[32];
  struct tm tm;
  getLocalTime(&tm);
  timeToString(&tm, t, sizeof(t));
  res["time"] = t;
}

void get_boot_time(JsonObject res) {
  char b[32];
  timeToString(&boot_time, b, sizeof(b));
  res["boot"] = b;
}

void get_state(JsonObject res) {
  JsonArray array = res.createNestedArray("pumps");
  for(int i = 0; i < NRPUMPS; i++) {
    JsonObject p = array.createNestedObject();
    p["pump"] = i + 1;
    p["minute"] = pumps[i].data->trigger_min;
    p["ml_per_us"] = pumps[i].data->ml_per_us;
    p["dosed"] = pumps[i].ml_dosed;
    p["disabled"] = pumps[i].disabled_for;
    if(pumps[i].state == Pump::state_t::RUNNING) {
      unsigned long running_for = micros() - pumps[i].run_start;
      p["running"] = running_for;
      p["us"] = pumps[i].run_for;
    }
    JsonArray schedule = p.createNestedArray("schedule");
    for(int j = 0; j < 24; j++) {
      schedule.add(pumps[i].data->schedule[j]);
    }
  }
}

void disable(JsonArray pumps, JsonVariant disable, JsonObject res) {
  int dis = disable.as<int>();
  for (JsonVariant pump_id : pumps) {
    if(pump_by_id(pump_id.as<int>()) == nullptr) {
      res["msg"] = "error";
      res["error"] = "Invalid pump";
      return;
    }
  }
  if(!disable.is<int>() || dis < 0) {
    res["msg"] = "error";
    res["error"] = "Invalid number of periods";
    return;
  }
  for (JsonVariant pump_id : pumps) {
    Pump* pump = pump_by_id(pump_id);
    pump->disabled_for = dis;
  }
  res["msg"] = "ok";
}

void set_sched(JsonArray pumps, JsonArray sched, JsonObject res) {
  if(sched.size() != 24) {
    res["msg"] = "error";
    res["error"] = "Invalid schedule size";
    return;
  }
  if(pumps.size() == 0) {
    res["msg"] = "error";
    res["error"] = "No pumps selected";
    return;
  }
  for (JsonVariant pump_id : pumps) {
    if(pump_by_id(pump_id.as<int>()) == nullptr) {
      res["msg"] = "error";
      res["error"] = "Invalid pump";
      return;
    }
  }
  for (JsonVariant value : sched) {
    if(!value.is<double>()) {
      res["msg"] = "error";
      res["error"] = "Invalid schedule entry";
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
  res["msg"] = "ok";
}

void set_spread(JsonArray minutes, JsonObject res) {
  if(minutes.size() != NRPUMPS) {
    res["msg"] = "error";
    res["error"] = "Invalid number of minutes";
    return;
  }
  for (int i = 0; i < minutes.size(); i++) {
    int minute = minutes[i].as<int>();
    if(!minutes[i].is<int>() && minute >= 0 && minute < 60) {
      res["msg"] = "error";
      res["error"] = "Invalid minute";
      return;
    }
  }
  for (int i = 0; i < minutes.size(); i++) {
    pumps[i].data->trigger_min = minutes[i];
  }
  save();
  res["msg"] = "ok";
}

void onJson(JsonObject obj) {
  DynamicJsonDocument doc(4096);
  JsonObject res = doc.to<JsonObject>();
  if (obj.containsKey("id")) {
    res["ack"] = obj["id"];
  }
  if(!obj["msg"].is<char*>()) {
    res["msg"] = "ack";
  } else if (   strcmp(obj["msg"], "get_time") == 0) {
    get_time(res);
  } else if (   strcmp(obj["msg"], "run_pump") == 0) {
    int id           = obj["pump"];
    unsigned long us = obj["us"];
    double ml        = obj["ml"];
    run_pump(id, us, ml, res);
  } else if (   strcmp(obj["msg"], "stop_pump") == 0) {
    int id           = obj["pump"];
    stop_pump(id, res);
  } else if(    strcmp(obj["msg"], "disable") == 0) {
    JsonArray pumps  = obj["pumps"];
    JsonVariant dis  = obj["disable"];
    disable(pumps, dis, res);
  } else if(    strcmp(obj["msg"], "set_cal") == 0) {
    int id           = obj["pump"];
    double ml        = obj["ml"];
    double us        = obj["us"];
    set_cal(id, ml, us, res);
  } else if(    strcmp(obj["msg"], "set_spread") == 0) {
    JsonArray minutes  = obj["minutes"];
    set_spread(minutes, res);
  } else if(    strcmp(obj["msg"], "set_sched") == 0) {
    JsonArray pumps  = obj["pumps"];
    JsonArray sched  = obj["schedule"];
    set_sched(pumps, sched, res);
  } else if(    strcmp(obj["msg"], "get_state") == 0) {
    get_time(res);
    get_boot_time(res);
    get_state(res);
  } else {
    res["msg"] = "ack";
  }
  sendJson(res);
}

void onWsMsg(WebsocketsMessage message) {
  StaticJsonDocument<capacity> doc;
  DeserializationError error = deserializeJson(doc, message.data());
  JsonObject obj = doc.as<JsonObject>();
  if (error || obj.isNull()) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.c_str());
    Serial.print("Got Message: ");
    Serial.println(message.data());
    return;
  }
  serializeJson(obj, Serial);
  Serial.println();
  onJson(obj);
}

void onWsEvent(WebsocketsEvent event, String data) {
  if (event == WebsocketsEvent::ConnectionOpened) {
    Serial.println("Connnection Opened");
  } else if (event == WebsocketsEvent::ConnectionClosed) {
    Serial.println("Connnection Closed");
  } else if (event == WebsocketsEvent::GotPing) {
    Serial.println("Got a Ping!");
  } else if (event == WebsocketsEvent::GotPong) {
    Serial.println("Got a Pong!");
  }
}

void sync_time() {
  time_t before = time(nullptr);
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", ntpServer);
  time_t after = time(nullptr);
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
  Serial.println("\nHello lexpump");
  WiFi.setHostname("lexpump");

  client.onMessage(onWsMsg);
  client.onEvent(onWsEvent);

  Config.ticker = true;
  Config.tickerPort = 13;
  Config.tickerOn = HIGH;
  Config.autoReconnect = true;
  Portal.config(Config);

  if (Portal.begin()) {
    Serial.println("WiFi connected: " + WiFi.localIP().toString());
    sync_time();
  }
}

void reconnect_loop() {
  static unsigned long prev = 0;
  unsigned long now = micros();
  if((now - prev) >= 2 * MICRO) {
    prev = now;
    Serial.println("Reconnecting");
    client.connect(server_host, server_port, "/ws");
  }
}

void sync_time_loop() {
  static unsigned long prev = 0;
  unsigned long now = micros();
  if((now - prev) > 5 * 60 * MICRO) {
    prev = now;
    sync_time();
  }
}

byte lastTimeSync = -1;
void loop() {
  Portal.handleClient();
  sync_time_loop();
  if (client.available()) {
    client.poll();
  } else {
    reconnect_loop();
  }
  struct tm tm;
  getLocalTime(&tm);
  if ((tm.tm_year + 1900) < 2000) {
    Serial.println("Waiting for time");
    delay(1000);
    return;
  }
  if ((boot_time.tm_year + 1900) < 2000) {
    boot_time = tm;
  }

  tm.tm_hour  = tm.tm_min % 24;
  tm.tm_min = tm.tm_sec;

  for(int i = 0; i < NRPUMPS; i++) {
    pumps[i].update(tm);
  }
  delay(1);
}
