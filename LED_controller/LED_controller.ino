// ============================================================
//  LED-контроллер с системой слоёв, пресетов и анимаций
//  Версия 2.8 (добавлена команда sf)
// ============================================================

#include <FastLED.h>
#include <LittleFS.h>
#include <Preferences.h>

// ============================================================
//  КОНФИГУРАЦИЯ
// ============================================================
#define NUM_LEDS         288
#define DATA_PIN         16
#define LED_TYPE         WS2812B
#define COLOR_ORDER      GRB

#define MAX_LAYERS       20
#define MAX_POINTS       40
#define MAX_FRAMES       100
#define MAX_PRESET_NAME  32
#define STATE_SLOTS      16

CRGB leds[NUM_LEDS];

// ============================================================
//  ТИПЫ И СТРУКТУРЫ
// ============================================================
enum EffectType { EF_NONE, EF_RAINBOW, EF_MONO, EF_DOT, EF_COMET, EF_INTERPOLATE };

struct InterpPoint {
    uint16_t pos;
    CRGB color;
};

struct EffectParams {
    uint8_t hueStep;
    uint8_t speedStep;
    uint8_t cometStep;
    CRGB color;
    int paletteSize;
    CRGB palette[10];
    int pointCount;
    InterpPoint points[MAX_POINTS];
    int32_t state[STATE_SLOTS];
    uint32_t phase;   // 0xFFFFFFFF = авто
};

struct Layer {
    EffectType type;
    void (*draw)(Layer* layer, CRGB* buffer);
    int start;
    int end;
    int delay;
    unsigned long lastUpdate;
    bool (*mask)(int index);
    EffectParams params;
    Layer() {
        type = EF_NONE;
        draw = nullptr;
        start = 0; end = 0; delay = 20;
        lastUpdate = 0;
        mask = nullptr;
        memset(&params, 0, sizeof(params));
        params.phase = 0xFFFFFFFF;
    }
};

struct SerializedLayer {
    EffectType type;
    int start;
    int end;
    int delay;
    EffectParams params;
    char maskName[16];
};

struct FileHeader {
    uint8_t version;      // 2
    uint8_t type;         // 0-static, 1-anim_fixed, 2-anim_var
    uint16_t layerCount;
};

struct AnimFixedHeader {
    uint16_t fps;
    uint8_t loop;
    uint16_t frameCount;
};
struct AnimVarHeader {
    uint8_t loop;
    uint16_t frameCount;
};

Layer layers[MAX_LAYERS];
int layerCount = 0;
uint8_t brightness = 50;
bool effectsPaused = false;

struct AnimationPlayer {
    bool loaded;
    uint8_t type;
    uint16_t layerCount;
    uint16_t frameCount;
    uint8_t loop;
    uint16_t fps;
    uint16_t* delays;
    SerializedLayer** frames;
    int currentFrame;
    unsigned long frameStartTime;
    bool playing;
    bool paused;
    char fileName[MAX_PRESET_NAME];
    bool dirty;
};
AnimationPlayer player;

bool editMode = false;
bool editDirty = false;

bool checkerMask(int index) { return (index % 4) < 2; }
bool evenMask(int index)   { return (index % 2) == 0; }
bool thirdMask(int index)  { return (index % 3) == 0; }
bool allMask(int index)    { return true; }

CRGB firePalette[] = {
    CRGB(255, 0, 0), CRGB(255, 100, 0), CRGB(255, 160, 0),
    CRGB(200, 160, 0), CRGB(140, 100, 0), CRGB(80, 60, 0),
    CRGB(20, 10, 0)
};
#define FIRE_PALETTE_SIZE 7

CRGB icePalette[] = {
    CRGB(0, 255, 255), CRGB(0, 200, 255), CRGB(100, 200, 255),
    CRGB(200, 230, 255), CRGB(255, 255, 255)
};
#define ICE_PALETTE_SIZE 5

// ============================================================
//  ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
// ============================================================
int extractNumber(String s) {
    int num = 0;
    for (int i = 0; i < s.length(); i++) {
        char c = s.charAt(i);
        if (isDigit(c)) num = num * 10 + (c - '0');
        else break;
    }
    return num;
}

bool parseNumbers(String s, int* arr, int count) {
    int idx = 0, pos = 0;
    while (pos < s.length() && idx < count) {
        while (pos < s.length() && s.charAt(pos) == ' ') pos++;
        if (pos >= s.length()) break;
        int val = 0;
        bool hasDigit = false;
        while (pos < s.length() && isDigit(s.charAt(pos))) {
            val = val * 10 + (s.charAt(pos) - '0');
            pos++;
            hasDigit = true;
        }
        if (!hasDigit) break;
        arr[idx++] = val;
    }
    return idx == count;
}

bool parseRGB(String s, CRGB& color) {
    int vals[3];
    if (parseNumbers(s, vals, 3)) {
        color = CRGB(vals[0], vals[1], vals[2]);
        return true;
    }
    return false;
}

bool (*getMaskByName(const char* name))(int) {
    if (strcmp(name, "checker") == 0) return checkerMask;
    if (strcmp(name, "even") == 0) return evenMask;
    if (strcmp(name, "third") == 0) return thirdMask;
    if (strcmp(name, "all") == 0) return allMask;
    return nullptr;
}

String getMaskName(Layer* layer) {
    if (layer->mask == checkerMask) return "checker";
    if (layer->mask == evenMask) return "even";
    if (layer->mask == thirdMask) return "third";
    if (layer->mask == allMask) return "all";
    return "";
}

EffectType getTypeFromString(String typeStr) {
    if (typeStr == "rainbow") return EF_RAINBOW;
    if (typeStr == "mono") return EF_MONO;
    if (typeStr == "dot") return EF_DOT;
    if (typeStr == "comet") return EF_COMET;
    if (typeStr == "interpolate") return EF_INTERPOLATE;
    return EF_NONE;
}

// ============================================================
//  ФУНКЦИИ РИСОВАНИЯ ЭФФЕКТОВ
// ============================================================
void drawRainbow(Layer* layer, CRGB* buffer) {
    unsigned long now = millis();
    if (!effectsPaused && (now - layer->lastUpdate >= layer->delay)) {
        layer->lastUpdate = now;
        layer->params.state[0] += layer->params.speedStep;
    }
    uint8_t hueOffset = (uint8_t)layer->params.state[0];
    uint8_t hueStep   = layer->params.hueStep;
    for (int i = layer->start; i <= layer->end; i++) {
        if (layer->mask && !layer->mask(i)) continue;
        uint8_t hue = hueOffset + (i - layer->start) * hueStep;
        buffer[i] = CHSV(hue, 255, 255);
    }
}

void drawMono(Layer* layer, CRGB* buffer) {
    CRGB color = layer->params.color;
    for (int i = layer->start; i <= layer->end; i++) {
        if (layer->mask && !layer->mask(i)) continue;
        buffer[i] = color;
    }
}

void drawDot(Layer* layer, CRGB* buffer) {
    unsigned long now = millis();
    if (!effectsPaused && (now - layer->lastUpdate >= layer->delay)) {
        layer->lastUpdate = now;
        layer->params.state[0]++;
        if (layer->params.state[0] > layer->end) layer->params.state[0] = layer->start;
    }
    int pos = (int)layer->params.state[0];
    if (pos >= layer->start && pos <= layer->end) {
        if (!layer->mask || layer->mask(pos)) {
            buffer[pos] = CRGB::White;
        }
    }
}

void drawComet(Layer* layer, CRGB* buffer) {
    unsigned long now = millis();
    if (!effectsPaused && (now - layer->lastUpdate >= layer->delay)) {
        layer->lastUpdate = now;
        layer->params.state[0] += layer->params.cometStep;
        if (layer->params.state[0] > layer->end) layer->params.state[0] = layer->start;
        if (layer->params.state[0] < layer->start) layer->params.state[0] = layer->end;
    }
    int head = (int)layer->params.state[0];
    int paletteSize = layer->params.paletteSize;
    for (int i = 0; i < paletteSize; i++) {
        int pos = head - i;
        if (pos >= layer->start && pos <= layer->end) {
            if (!layer->mask || layer->mask(pos)) {
                buffer[pos] = layer->params.palette[i];
            }
        }
    }
}

void drawInterpolate(Layer* layer, CRGB* buffer) {
    if (layer->params.pointCount < 2) return;
    for (int i = layer->start; i <= layer->end; i++) {
        bool exact = false;
        for (int p = 0; p < layer->params.pointCount; p++) {
            if (layer->params.points[p].pos == i) {
                if (!layer->mask || layer->mask(i)) {
                    buffer[i] = layer->params.points[p].color;
                }
                exact = true;
                break;
            }
        }
        if (exact) continue;
        int leftIdx = -1, rightIdx = -1;
        for (int p = 0; p < layer->params.pointCount; p++) {
            if (layer->params.points[p].pos < i) {
                if (leftIdx == -1 || layer->params.points[p].pos > layer->params.points[leftIdx].pos) {
                    leftIdx = p;
                }
            }
            if (layer->params.points[p].pos > i) {
                if (rightIdx == -1 || layer->params.points[p].pos < layer->params.points[rightIdx].pos) {
                    rightIdx = p;
                }
            }
        }
        if (leftIdx != -1 && rightIdx != -1) {
            uint16_t leftPos = layer->params.points[leftIdx].pos;
            uint16_t rightPos = layer->params.points[rightIdx].pos;
            CRGB leftColor = layer->params.points[leftIdx].color;
            CRGB rightColor = layer->params.points[rightIdx].color;
            float t = (float)(i - leftPos) / (rightPos - leftPos);
            uint8_t r = leftColor.r + (rightColor.r - leftColor.r) * t;
            uint8_t g = leftColor.g + (rightColor.g - leftColor.g) * t;
            uint8_t b = leftColor.b + (rightColor.b - leftColor.b) * t;
            if (!layer->mask || layer->mask(i)) {
                buffer[i] = CRGB(r, g, b);
            }
        }
    }
}

// ============================================================
//  УПРАВЛЕНИЕ СЛОЯМИ
// ============================================================
int addLayer(EffectType type, int start, int end, int delay, EffectParams params, bool (*mask)(int) = nullptr) {
    if (layerCount >= MAX_LAYERS) {
        Serial.println("Ошибка: достигнут максимум слоёв");
        return -1;
    }
    Layer* layer = &layers[layerCount];
    layer->type = type;
    layer->start = start;
    layer->end = end;
    layer->delay = delay;
    layer->lastUpdate = 0;
    layer->mask = mask;
    layer->params = params;
    switch (type) {
        case EF_RAINBOW:      layer->draw = drawRainbow; break;
        case EF_MONO:         layer->draw = drawMono; break;
        case EF_DOT:          layer->draw = drawDot; break;
        case EF_COMET:        layer->draw = drawComet; break;
        case EF_INTERPOLATE:  layer->draw = drawInterpolate; break;
        default: return -1;
    }
    layerCount++;
    return layerCount - 1;
}

int insertLayer(int index, EffectType type, int start, int end, int delay, EffectParams params, bool (*mask)(int) = nullptr) {
    if (layerCount >= MAX_LAYERS) {
        Serial.println("Ошибка: достигнут максимум слоёв");
        return -1;
    }
    if (index < 0 || index > layerCount) {
        Serial.println("Ошибка: индекс вне диапазона");
        return -1;
    }
    for (int i = layerCount; i > index; i--) {
        layers[i] = layers[i - 1];
    }
    Layer* layer = &layers[index];
    layer->type = type;
    layer->start = start;
    layer->end = end;
    layer->delay = delay;
    layer->lastUpdate = 0;
    layer->mask = mask;
    layer->params = params;
    switch (type) {
        case EF_RAINBOW:      layer->draw = drawRainbow; break;
        case EF_MONO:         layer->draw = drawMono; break;
        case EF_DOT:          layer->draw = drawDot; break;
        case EF_COMET:        layer->draw = drawComet; break;
        case EF_INTERPOLATE:  layer->draw = drawInterpolate; break;
        default: return -1;
    }
    layerCount++;
    return index;
}

void removeLayer(int index) {
    if (index < 0 || index >= layerCount) return;
    for (int i = index; i < layerCount - 1; i++) layers[i] = layers[i + 1];
    layerCount--;
}

void clearLayers() {
    layerCount = 0;
}

// ============================================================
//  ПРИМЕНЕНИЕ КАДРА
// ============================================================
void applySerializedLayer(const SerializedLayer& sl, int index) {
    if (index < layerCount) {
        Layer* layer = &layers[index];
        if (layer->type == sl.type) {
            if (sl.params.phase != 0xFFFFFFFF) {
                layer->params = sl.params;
                layer->mask = getMaskByName(sl.maskName);
                layer->start = sl.start;
                layer->end = sl.end;
                layer->delay = sl.delay;
            } else {
                int32_t savedState[STATE_SLOTS];
                memcpy(savedState, layer->params.state, sizeof(savedState));
                layer->params = sl.params;
                memcpy(layer->params.state, savedState, sizeof(savedState));
                layer->mask = getMaskByName(sl.maskName);
                layer->start = sl.start;
                layer->end = sl.end;
                layer->delay = sl.delay;
            }
        } else {
            layer->type = sl.type;
            layer->params = sl.params;
            layer->mask = getMaskByName(sl.maskName);
            layer->start = sl.start;
            layer->end = sl.end;
            layer->delay = sl.delay;
            if (sl.params.phase == 0xFFFFFFFF) {
                memset(layer->params.state, 0, sizeof(layer->params.state));
            }
            switch (sl.type) {
                case EF_RAINBOW: layer->draw = drawRainbow; break;
                case EF_MONO: layer->draw = drawMono; break;
                case EF_DOT: layer->draw = drawDot; break;
                case EF_COMET: layer->draw = drawComet; break;
                case EF_INTERPOLATE: layer->draw = drawInterpolate; break;
                default: layer->draw = nullptr;
            }
            layer->lastUpdate = millis();
        }
    } else {
        addLayer(sl.type, sl.start, sl.end, sl.delay, sl.params, getMaskByName(sl.maskName));
        if (sl.params.phase == 0xFFFFFFFF) {
            memset(layers[layerCount-1].params.state, 0, sizeof(layers[layerCount-1].params.state));
        }
        layers[layerCount-1].lastUpdate = millis();
    }
}

void applyFrame(int frameIndex) {
    if (!player.loaded) {
        Serial.println("applyFrame: player не загружен");
        return;
    }
    if (frameIndex < 0 || frameIndex >= player.frameCount) {
        Serial.println("applyFrame: неверный индекс кадра");
        return;
    }
    if (!player.frames) {
        Serial.println("applyFrame: player.frames == NULL");
        return;
    }
    if (!player.frames[frameIndex]) {
        Serial.println("applyFrame: player.frames[frameIndex] == NULL");
        return;
    }

    for (int i = 0; i < player.layerCount; i++) {
        SerializedLayer* sl = &player.frames[frameIndex][i];
        if (i < layerCount) {
            Layer* layer = &layers[i];
            if (layer->type == sl->type) {
                if (sl->params.phase != 0xFFFFFFFF) {
                    layer->params = sl->params;
                    layer->mask = getMaskByName(sl->maskName);
                    layer->start = sl->start;
                    layer->end = sl->end;
                    layer->delay = sl->delay;
                } else {
                    int32_t savedState[STATE_SLOTS];
                    memcpy(savedState, layer->params.state, sizeof(savedState));
                    layer->params = sl->params;
                    memcpy(layer->params.state, savedState, sizeof(savedState));
                    layer->mask = getMaskByName(sl->maskName);
                    layer->start = sl->start;
                    layer->end = sl->end;
                    layer->delay = sl->delay;
                }
            } else {
                layer->type = sl->type;
                layer->params = sl->params;
                layer->mask = getMaskByName(sl->maskName);
                layer->start = sl->start;
                layer->end = sl->end;
                layer->delay = sl->delay;
                if (sl->params.phase == 0xFFFFFFFF) {
                    memset(layer->params.state, 0, sizeof(layer->params.state));
                }
                switch (sl->type) {
                    case EF_RAINBOW: layer->draw = drawRainbow; break;
                    case EF_MONO: layer->draw = drawMono; break;
                    case EF_DOT: layer->draw = drawDot; break;
                    case EF_COMET: layer->draw = drawComet; break;
                    case EF_INTERPOLATE: layer->draw = drawInterpolate; break;
                    default: layer->draw = nullptr;
                }
                layer->lastUpdate = millis();
            }
        } else {
            addLayer(sl->type, sl->start, sl->end, sl->delay, sl->params, getMaskByName(sl->maskName));
            if (sl->params.phase == 0xFFFFFFFF) {
                memset(layers[layerCount-1].params.state, 0, sizeof(layers[layerCount-1].params.state));
            }
            layers[layerCount-1].lastUpdate = millis();
        }
    }
    while (layerCount > player.layerCount) {
        removeLayer(layerCount - 1);
    }
    player.currentFrame = frameIndex;
}

// ============================================================
//  РАБОТА С ФАЙЛАМИ (LittleFS)
// ============================================================
bool initFS() {
    if (!LittleFS.begin(true)) {
        Serial.println("Ошибка монтирования LittleFS");
        return false;
    }
    Serial.println("LittleFS смонтирована");
    return true;
}

bool saveStatic(const char* name) {
    String path = "/" + String(name) + ".bin";
    File file = LittleFS.open(path, "w");
    if (!file) {
        Serial.println("Не удалось создать файл");
        return false;
    }
    FileHeader header;
    header.version = 2;
    header.type = 0;
    header.layerCount = layerCount;
    file.write((uint8_t*)&header, sizeof(header));
    for (int i = 0; i < layerCount; i++) {
        SerializedLayer sl;
        sl.type = layers[i].type;
        sl.start = layers[i].start;
        sl.end = layers[i].end;
        sl.delay = layers[i].delay;
        sl.params = layers[i].params;
        String maskName = getMaskName(&layers[i]);
        strncpy(sl.maskName, maskName.c_str(), 15);
        sl.maskName[15] = '\0';
        file.write((uint8_t*)&sl, sizeof(sl));
    }
    file.close();
    Serial.print("Пресет сохранён: ");
    Serial.println(name);
    return true;
}

bool loadPreset(const char* name) {
    String path = "/" + String(name) + ".bin";
    if (!LittleFS.exists(path)) {
        Serial.println("Файл не найден");
        return false;
    }
    File file = LittleFS.open(path, "r");
    if (!file) {
        Serial.println("Не удалось открыть файл");
        return false;
    }

    Serial.println("Чтение заголовка...");
    FileHeader header;
    if (file.read((uint8_t*)&header, sizeof(header)) != sizeof(header)) {
        Serial.println("Ошибка чтения заголовка");
        file.close();
        return false;
    }
    if (header.version != 2) {
        Serial.println("Неверная версия файла (требуется 2)");
        file.close();
        return false;
    }

    Serial.print("Тип: ");
    Serial.println(header.type);
    Serial.print("Количество слоёв: ");
    Serial.println(header.layerCount);

    if (player.loaded) {
        if (player.frames) {
            for (int f = 0; f < player.frameCount; f++) {
                if (player.frames[f]) free(player.frames[f]);
            }
            free(player.frames);
        }
        if (player.delays) free(player.delays);
        memset(&player, 0, sizeof(player));
    }

    player.loaded = true;
    player.type = header.type;
    player.layerCount = header.layerCount;
    strncpy(player.fileName, name, MAX_PRESET_NAME-1);
    player.fileName[MAX_PRESET_NAME-1] = '\0';
    player.dirty = false;
    player.playing = false;
    player.paused = false;

    if (header.type == 0) {
        Serial.println("Загрузка статики...");
        player.frameCount = 1;
        player.loop = 0;
        player.frames = (SerializedLayer**)malloc(sizeof(SerializedLayer*));
        if (!player.frames) { Serial.println("Ошибка malloc для frames"); file.close(); return false; }
        player.frames[0] = (SerializedLayer*)malloc(player.layerCount * sizeof(SerializedLayer));
        if (!player.frames[0]) { Serial.println("Ошибка malloc для слоёв"); free(player.frames); player.frames = NULL; file.close(); return false; }
        for (int i = 0; i < player.layerCount; i++) {
            if (file.read((uint8_t*)&(player.frames[0][i]), sizeof(SerializedLayer)) != sizeof(SerializedLayer)) {
                Serial.println("Ошибка чтения слоя");
                file.close();
                return false;
            }
        }
        Serial.println("Применение кадра 0...");
        applyFrame(0);
        Serial.println("Загружена статика (с сохранением состояния)");
    } else if (header.type == 1) {
        Serial.println("Загрузка anim_fixed...");
        AnimFixedHeader animHeader;
        if (file.read((uint8_t*)&animHeader, sizeof(animHeader)) != sizeof(animHeader)) {
            Serial.println("Ошибка чтения заголовка анимации");
            file.close();
            return false;
        }
        player.fps = animHeader.fps;
        player.loop = animHeader.loop;
        player.frameCount = animHeader.frameCount;
        player.frames = (SerializedLayer**)malloc(player.frameCount * sizeof(SerializedLayer*));
        if (!player.frames) { Serial.println("Ошибка malloc frames"); file.close(); return false; }
        for (int f = 0; f < player.frameCount; f++) {
            player.frames[f] = (SerializedLayer*)malloc(player.layerCount * sizeof(SerializedLayer));
            if (!player.frames[f]) {
                Serial.print("Ошибка malloc для кадра "); Serial.println(f);
                for (int k = 0; k < f; k++) free(player.frames[k]);
                free(player.frames);
                player.frames = NULL;
                file.close();
                return false;
            }
            for (int i = 0; i < player.layerCount; i++) {
                if (file.read((uint8_t*)&(player.frames[f][i]), sizeof(SerializedLayer)) != sizeof(SerializedLayer)) {
                    Serial.println("Ошибка чтения слоя");
                    file.close();
                    return false;
                }
            }
        }
        player.currentFrame = 0;
        player.frameStartTime = 0;
        Serial.println("Применение кадра 0...");
        applyFrame(0);
        Serial.println("Загружена анимация (fixed) с сохранением состояния");
    } else if (header.type == 2) {
        Serial.println("Загрузка anim_var...");
        AnimVarHeader animHeader;
        if (file.read((uint8_t*)&animHeader, sizeof(animHeader)) != sizeof(animHeader)) {
            Serial.println("Ошибка чтения заголовка анимации");
            file.close();
            return false;
        }
        player.loop = animHeader.loop;
        player.frameCount = animHeader.frameCount;
        player.delays = (uint16_t*)malloc(player.frameCount * sizeof(uint16_t));
        if (!player.delays) {
            Serial.println("Ошибка malloc delays");
            file.close();
            return false;
        }
        if (file.read((uint8_t*)player.delays, player.frameCount * sizeof(uint16_t)) != player.frameCount * sizeof(uint16_t)) {
            Serial.println("Ошибка чтения задержек");
            free(player.delays);
            player.delays = NULL;
            file.close();
            return false;
        }
        player.frames = (SerializedLayer**)malloc(player.frameCount * sizeof(SerializedLayer*));
        if (!player.frames) {
            Serial.println("Ошибка malloc frames");
            free(player.delays);
            player.delays = NULL;
            file.close();
            return false;
        }
        for (int f = 0; f < player.frameCount; f++) {
            player.frames[f] = (SerializedLayer*)malloc(player.layerCount * sizeof(SerializedLayer));
            if (!player.frames[f]) {
                Serial.print("Ошибка malloc для кадра "); Serial.println(f);
                for (int k = 0; k < f; k++) free(player.frames[k]);
                free(player.frames);
                player.frames = NULL;
                free(player.delays);
                player.delays = NULL;
                file.close();
                return false;
            }
            for (int i = 0; i < player.layerCount; i++) {
                if (file.read((uint8_t*)&(player.frames[f][i]), sizeof(SerializedLayer)) != sizeof(SerializedLayer)) {
                    Serial.println("Ошибка чтения слоя");
                    file.close();
                    return false;
                }
            }
        }
        player.currentFrame = 0;
        player.frameStartTime = 0;
        Serial.println("Применение кадра 0...");
        applyFrame(0);
        Serial.println("Загружена анимация (var) с сохранением состояния");
    } else {
        Serial.println("Неизвестный тип файла");
        file.close();
        return false;
    }
    file.close();
    stopAnimation();
    Serial.println("Загрузка завершена успешно.");
    return true;
}

bool saveAnimation(const char* name) {
    if (!player.loaded) {
        Serial.println("Нет загруженной анимации");
        return false;
    }
    String path = "/" + String(name) + ".bin";
    File file = LittleFS.open(path, "w");
    if (!file) {
        Serial.println("Не удалось создать файл");
        return false;
    }
    FileHeader header;
    header.version = 2;
    header.type = player.type;
    header.layerCount = player.layerCount;
    file.write((uint8_t*)&header, sizeof(header));

    if (player.type == 0) {
        for (int i = 0; i < player.layerCount; i++) {
            file.write((uint8_t*)&(player.frames[0][i]), sizeof(SerializedLayer));
        }
    } else if (player.type == 1) {
        AnimFixedHeader animHeader;
        animHeader.fps = player.fps;
        animHeader.loop = player.loop;
        animHeader.frameCount = player.frameCount;
        file.write((uint8_t*)&animHeader, sizeof(animHeader));
        for (int f = 0; f < player.frameCount; f++) {
            for (int i = 0; i < player.layerCount; i++) {
                file.write((uint8_t*)&(player.frames[f][i]), sizeof(SerializedLayer));
            }
        }
    } else if (player.type == 2) {
        AnimVarHeader animHeader;
        animHeader.loop = player.loop;
        animHeader.frameCount = player.frameCount;
        file.write((uint8_t*)&animHeader, sizeof(animHeader));
        file.write((uint8_t*)player.delays, player.frameCount * sizeof(uint16_t));
        for (int f = 0; f < player.frameCount; f++) {
            for (int i = 0; i < player.layerCount; i++) {
                file.write((uint8_t*)&(player.frames[f][i]), sizeof(SerializedLayer));
            }
        }
    }
    file.close();
    Serial.print("Анимация сохранена: ");
    Serial.println(name);
    player.dirty = false;
    return true;
}

void deleteFile(const char* name) {
    String path = "/" + String(name) + ".bin";
    if (LittleFS.remove(path)) {
        Serial.print("Файл удалён: ");
        Serial.println(name);
    } else {
        Serial.println("Не удалось удалить файл");
    }
}

void listFiles() {
    Serial.println("=== Файлы в папке / ===");
    File root = LittleFS.open("/");
    if (!root) {
        Serial.println("Не удалось открыть папку");
        return;
    }
    int count = 0;
    File file = root.openNextFile();
    while (file) {
        String name = file.name();
        if (name.endsWith(".bin")) {
            name = name.substring(0, name.length() - 4);
            Serial.print("  ");
            Serial.println(name);
            count++;
        }
        file = root.openNextFile();
    }
    if (count == 0) Serial.println("  (нет файлов)");
}

void printInfo(bool full) {
    if (!player.loaded) {
        Serial.println("Ничего не загружено");
        return;
    }
    Serial.println("=== Информация о пресете ===");
    Serial.print("Имя: "); Serial.println(player.fileName);
    Serial.print("Тип: ");
    if (player.type == 0) Serial.println("static");
    else if (player.type == 1) Serial.println("anim_fixed");
    else Serial.println("anim_var");
    Serial.print("Слоёв: "); Serial.println(player.layerCount);
    Serial.print("Кадров: "); Serial.println(player.frameCount);
    Serial.print("Зациклить: "); Serial.println(player.loop ? "да" : "нет");
    if (player.type == 1) {
        Serial.print("FPS: "); Serial.println(player.fps);
    } else if (player.type == 2) {
        Serial.print("Задержки (мс): ");
        for (int i = 0; i < player.frameCount; i++) {
            Serial.print(player.delays[i]);
            Serial.print(" ");
        }
        Serial.println();
    }
    Serial.print("Текущий кадр: "); Serial.println(player.currentFrame);
    if (full) {
        Serial.println("=== Кадры ===");
        for (int f = 0; f < player.frameCount; f++) {
            Serial.print("Кадр "); Serial.print(f); Serial.println(":");
            for (int i = 0; i < player.layerCount; i++) {
                SerializedLayer* sl = &player.frames[f][i];
                Serial.print("  Слой "); Serial.print(i);
                Serial.print(": тип ");
                switch (sl->type) {
                    case EF_NONE: Serial.print("NONE"); break;
                    case EF_RAINBOW: Serial.print("RAINBOW"); break;
                    case EF_MONO: Serial.print("MONO"); break;
                    case EF_DOT: Serial.print("DOT"); break;
                    case EF_COMET: Serial.print("COMET"); break;
                    case EF_INTERPOLATE: Serial.print("INTERPOLATE"); break;
                    default: Serial.print("?");
                }
                Serial.print(" start="); Serial.print(sl->start);
                Serial.print(" end="); Serial.print(sl->end);
                Serial.print(" delay="); Serial.print(sl->delay);
                Serial.print(" phase="); Serial.print(sl->params.phase);
                Serial.print(" mask="); Serial.println(sl->maskName);
            }
        }
    }
}

// ============================================================
//  ВОСПРОИЗВЕДЕНИЕ АНИМАЦИИ
// ============================================================
void playAnimation() {
    if (!player.loaded || player.type == 0) {
        Serial.println("Нет загруженной анимации или это статика");
        return;
    }
    player.playing = true;
    player.paused = false;
    player.currentFrame = 0;
    player.frameStartTime = millis();
    applyFrame(0);
    Serial.println("Воспроизведение запущено");
}

void stopAnimation() {
    player.playing = false;
    player.paused = false;
    Serial.println("Воспроизведение остановлено");
}

void pauseAnimation() {
    if (player.playing) {
        player.paused = !player.paused;
        if (player.paused) {
            Serial.println("Пауза");
        } else {
            player.frameStartTime = millis() - (player.frameStartTime ? millis() - player.frameStartTime : 0);
            Serial.println("Продолжено");
        }
    }
}

void nextFrame() {
    if (!player.loaded) {
        Serial.println("Нет загруженного пресета");
        return;
    }
    if (player.type == 0) {
        Serial.println("Это статика, переход по кадрам невозможен");
        return;
    }
    int next = (player.currentFrame + 1) % player.frameCount;
    applyFrame(next);
    Serial.print("Переход на кадр "); Serial.println(next);
}

void prevFrame() {
    if (!player.loaded) {
        Serial.println("Нет загруженного пресета");
        return;
    }
    if (player.type == 0) {
        Serial.println("Это статика, переход по кадрам невозможен");
        return;
    }
    int prev = (player.currentFrame - 1 + player.frameCount) % player.frameCount;
    applyFrame(prev);
    Serial.print("Переход на кадр "); Serial.println(prev);
}

void gotoFrame(int num) {
    if (!player.loaded) {
        Serial.println("Нет загруженного пресета");
        return;
    }
    if (player.type == 0) {
        Serial.println("Это статика, переход по кадрам невозможен");
        return;
    }
    if (num < 0 || num >= player.frameCount) {
        Serial.println("Неверный номер кадра");
        return;
    }
    applyFrame(num);
    Serial.print("Переход на кадр "); Serial.println(num);
}

void updateAnimation() {
    if (!player.loaded || player.type == 0 || !player.playing || player.paused) return;
    unsigned long now = millis();
    unsigned long elapsed = now - player.frameStartTime;
    unsigned long interval = 0;
    if (player.type == 1) {
        interval = 1000 / player.fps;
    } else if (player.type == 2) {
        interval = player.delays[player.currentFrame];
    }
    if (elapsed >= interval) {
        int next = (player.currentFrame + 1);
        if (next >= player.frameCount) {
            if (player.loop) {
                next = 0;
            } else {
                player.playing = false;
                Serial.println("Анимация завершена");
                return;
            }
        }
        applyFrame(next);
        player.frameStartTime = now;
    }
}

// ============================================================
//  РЕДАКТОР АНИМАЦИЙ
// ============================================================
void enterEditMode() {
    if (!player.loaded) {
        Serial.println("Сначала загрузите пресет командой load");
        return;
    }
    editMode = true;
    editDirty = false;
    Serial.println("Режим редактирования активирован.");
    Serial.println("Для выхода: exit");
}

void exitEditMode() {
    if (!editMode) return;
    if (editDirty) {
        Serial.print("Есть несохранённые изменения. Сохранить? (y/n): ");
        saveAnimation(player.fileName);
        editDirty = false;
    }
    editMode = false;
    Serial.println("Режим редактирования завершён");
}

void addFrame() {
    if (!editMode) { Serial.println("Не в режиме редактирования"); return; }
    int newCount = player.frameCount + 1;
    SerializedLayer** newFrames = (SerializedLayer**)realloc(player.frames, newCount * sizeof(SerializedLayer*));
    if (!newFrames) { Serial.println("Ошибка памяти"); return; }
    player.frames = newFrames;
    player.frames[newCount-1] = (SerializedLayer*)malloc(player.layerCount * sizeof(SerializedLayer));
    for (int i = 0; i < player.layerCount; i++) {
        SerializedLayer* sl = &player.frames[newCount-1][i];
        if (i < layerCount) {
            sl->type = layers[i].type;
            sl->start = layers[i].start;
            sl->end = layers[i].end;
            sl->delay = layers[i].delay;
            sl->params = layers[i].params;
            String maskName = getMaskName(&layers[i]);
            strncpy(sl->maskName, maskName.c_str(), 15);
            sl->maskName[15] = '\0';
        } else {
            sl->type = EF_NONE;
            sl->start = 0; sl->end = 0; sl->delay = 20;
            memset(&sl->params, 0, sizeof(sl->params));
            sl->params.phase = 0xFFFFFFFF;
            sl->maskName[0] = '\0';
        }
    }
    player.frameCount = newCount;
    if (player.type == 2) {
        uint16_t* newDelays = (uint16_t*)realloc(player.delays, newCount * sizeof(uint16_t));
        if (newDelays) {
            player.delays = newDelays;
            player.delays[newCount-1] = 100;
        }
    }
    editDirty = true;
    Serial.println("Кадр добавлен");
}

void insertFrame(int index) {
    if (!editMode) { Serial.println("Не в режиме редактирования"); return; }
    if (index < 0 || index > player.frameCount) {
        Serial.println("Неверный индекс");
        return;
    }
    int newCount = player.frameCount + 1;
    SerializedLayer** newFrames = (SerializedLayer**)realloc(player.frames, newCount * sizeof(SerializedLayer*));
    if (!newFrames) { Serial.println("Ошибка памяти"); return; }
    player.frames = newFrames;
    for (int f = player.frameCount; f > index; f--) {
        player.frames[f] = player.frames[f-1];
    }
    player.frames[index] = (SerializedLayer*)malloc(player.layerCount * sizeof(SerializedLayer));
    for (int i = 0; i < player.layerCount; i++) {
        SerializedLayer* sl = &player.frames[index][i];
        if (i < layerCount) {
            sl->type = layers[i].type;
            sl->start = layers[i].start;
            sl->end = layers[i].end;
            sl->delay = layers[i].delay;
            sl->params = layers[i].params;
            String maskName = getMaskName(&layers[i]);
            strncpy(sl->maskName, maskName.c_str(), 15);
            sl->maskName[15] = '\0';
        } else {
            sl->type = EF_NONE;
            sl->start = 0; sl->end = 0; sl->delay = 20;
            memset(&sl->params, 0, sizeof(sl->params));
            sl->params.phase = 0xFFFFFFFF;
            sl->maskName[0] = '\0';
        }
    }
    player.frameCount = newCount;
    if (player.type == 2) {
        uint16_t* newDelays = (uint16_t*)realloc(player.delays, newCount * sizeof(uint16_t));
        if (newDelays) {
            player.delays = newDelays;
            for (int i = player.frameCount-1; i > index; i--) {
                player.delays[i] = player.delays[i-1];
            }
            player.delays[index] = 100;
        }
    }
    editDirty = true;
    Serial.println("Кадр вставлен");
}

void deleteFrame(int index) {
    if (!editMode) { Serial.println("Не в режиме редактирования"); return; }
    if (index < 0 || index >= player.frameCount || player.frameCount <= 1) {
        Serial.println("Нельзя удалить единственный кадр");
        return;
    }
    free(player.frames[index]);
    for (int f = index; f < player.frameCount-1; f++) {
        player.frames[f] = player.frames[f+1];
    }
    player.frameCount--;
    if (player.type == 2) {
        for (int i = index; i < player.frameCount; i++) {
            player.delays[i] = player.delays[i+1];
        }
        uint16_t* newDelays = (uint16_t*)realloc(player.delays, player.frameCount * sizeof(uint16_t));
        if (newDelays) player.delays = newDelays;
    }
    editDirty = true;
    Serial.println("Кадр удалён");
}

void replaceFrame(int index) {
    if (!editMode) { Serial.println("Не в режиме редактирования"); return; }
    if (index < 0 || index >= player.frameCount) {
        Serial.println("Неверный индекс");
        return;
    }
    for (int i = 0; i < player.layerCount; i++) {
        SerializedLayer* sl = &player.frames[index][i];
        if (i < layerCount) {
            sl->type = layers[i].type;
            sl->start = layers[i].start;
            sl->end = layers[i].end;
            sl->delay = layers[i].delay;
            sl->params = layers[i].params;
            String maskName = getMaskName(&layers[i]);
            strncpy(sl->maskName, maskName.c_str(), 15);
            sl->maskName[15] = '\0';
        } else {
            sl->type = EF_NONE;
            sl->start = 0; sl->end = 0; sl->delay = 20;
            memset(&sl->params, 0, sizeof(sl->params));
            sl->params.phase = 0xFFFFFFFF;
            sl->maskName[0] = '\0';
        }
    }
    editDirty = true;
    Serial.println("Кадр заменён");
}

void changeLayerCount(int newCount) {
    if (!editMode) { Serial.println("Не в режиме редактирования"); return; }
    if (newCount < 0 || newCount > MAX_LAYERS) {
        Serial.println("Недопустимое количество слоёв");
        return;
    }
    if (newCount == player.layerCount) {
        Serial.println("Количество слоёв уже такое");
        return;
    }
    if (newCount < player.layerCount) {
        Serial.print("Уменьшение количества слоёв приведёт к потере данных. Продолжить? (y/n): ");
    }
    for (int f = 0; f < player.frameCount; f++) {
        SerializedLayer* newLayerArray = (SerializedLayer*)malloc(newCount * sizeof(SerializedLayer));
        int copyCount = min(newCount, player.layerCount);
        for (int i = 0; i < copyCount; i++) {
            newLayerArray[i] = player.frames[f][i];
        }
        for (int i = player.layerCount; i < newCount; i++) {
            newLayerArray[i].type = EF_NONE;
            newLayerArray[i].start = 0;
            newLayerArray[i].end = 0;
            newLayerArray[i].delay = 20;
            memset(&newLayerArray[i].params, 0, sizeof(newLayerArray[i].params));
            newLayerArray[i].params.phase = 0xFFFFFFFF;
            newLayerArray[i].maskName[0] = '\0';
        }
        free(player.frames[f]);
        player.frames[f] = newLayerArray;
    }
    player.layerCount = newCount;
    editDirty = true;
    Serial.print("Количество слоёв изменено на "); Serial.println(newCount);
}

void setDelay(int index, uint16_t value) {
    if (!editMode) { Serial.println("Не в режиме редактирования"); return; }
    if (player.type != 2) { Serial.println("Не анимация с переменной задержкой"); return; }
    if (index < 0 || index >= player.frameCount) {
        Serial.println("Неверный индекс");
        return;
    }
    player.delays[index] = value;
    editDirty = true;
    Serial.println("Задержка обновлена");
}

void replaceDelays(String values) {
    if (!editMode) { Serial.println("Не в режиме редактирования"); return; }
    if (player.type != 2) { Serial.println("Не анимация с переменной задержкой"); return; }
    int arr[MAX_FRAMES];
    int count = 0;
    int pos = 0;
    while (pos < values.length() && count < MAX_FRAMES) {
        while (pos < values.length() && values.charAt(pos) == ' ') pos++;
        if (pos >= values.length()) break;
        int val = 0;
        bool hasDigit = false;
        while (pos < values.length() && isDigit(values.charAt(pos))) {
            val = val * 10 + (values.charAt(pos) - '0');
            pos++;
            hasDigit = true;
        }
        if (!hasDigit) break;
        arr[count++] = val;
    }
    if (count != player.frameCount) {
        Serial.println("Количество значений должно совпадать с числом кадров");
        return;
    }
    free(player.delays);
    player.delays = (uint16_t*)malloc(count * sizeof(uint16_t));
    for (int i = 0; i < count; i++) player.delays[i] = arr[i];
    editDirty = true;
    Serial.println("Задержки заменены");
}

void setPhase(int layerIdx, int phase) {
    if (!editMode) { Serial.println("Не в режиме редактирования"); return; }
    if (layerIdx < 0 || layerIdx >= player.layerCount) {
        Serial.println("Неверный индекс слоя");
        return;
    }
    int frame = player.currentFrame;
    SerializedLayer* sl = &player.frames[frame][layerIdx];
    if (phase == -1) {
        sl->params.phase = 0xFFFFFFFF;
    } else {
        sl->params.phase = (uint32_t)phase;
    }
    editDirty = true;
    Serial.print("Фаза слоя "); Serial.print(layerIdx); Serial.print(" в кадре "); Serial.print(frame); Serial.print(" установлена в ");
    if (phase == -1) Serial.println("auto");
    else Serial.println(phase);
}

void changeType(int newType) {
    if (!editMode) { Serial.println("Не в режиме редактирования"); return; }
    if (newType < 0 || newType > 2) {
        Serial.println("Тип должен быть 0 (static), 1 (fixed), 2 (var)");
        return;
    }
    if (newType == player.type) {
        Serial.println("Тип уже такой");
        return;
    }
    if (newType == 0) {
        if (player.frameCount > 1) {
            for (int f = 1; f < player.frameCount; f++) {
                free(player.frames[f]);
            }
            SerializedLayer** newFrames = (SerializedLayer**)realloc(player.frames, sizeof(SerializedLayer*));
            if (newFrames) player.frames = newFrames;
            player.frameCount = 1;
            if (player.type == 2 && player.delays) {
                free(player.delays);
                player.delays = nullptr;
            }
        }
        player.type = 0;
        Serial.println("Тип изменён на static");
    } else if (newType == 1) {
        player.type = 1;
        player.fps = 30;
        if (player.delays) {
            free(player.delays);
            player.delays = nullptr;
        }
        Serial.println("Тип изменён на anim_fixed (FPS=30)");
    } else if (newType == 2) {
        player.type = 2;
        player.delays = (uint16_t*)malloc(player.frameCount * sizeof(uint16_t));
        for (int i = 0; i < player.frameCount; i++) player.delays[i] = 100;
        Serial.println("Тип изменён на anim_var (задержки по умолчанию 100 мс)");
    }
    editDirty = true;
}

void setFPS(int fps) {
    if (!editMode) { Serial.println("Не в режиме редактирования"); return; }
    if (player.type != 1) {
        Serial.println("Только для anim_fixed");
        return;
    }
    if (fps < 1 || fps > 100) {
        Serial.println("FPS должен быть от 1 до 100");
        return;
    }
    player.fps = fps;
    editDirty = true;
    Serial.print("FPS установлен: "); Serial.println(fps);
}

void showEffects() {
    Serial.println("=== Доступные эффекты ===");
    Serial.println("  NONE        - пустой слой (ничего не рисует)");
    Serial.println("  RAINBOW     - радуга");
    Serial.println("    Параметры: hueStep (шаг оттенка между диодами), speedStep (скорость), state[0] (текущий оттенок)");
    Serial.println("    Создание: add rainbow start end delay [hueStep=1] [speedStep=1]");
    Serial.println("    Изменение: set <индекс> hueoffset <знач>, huestep <знач>, speedstep <знач>, delay <мс>, phase <знач>, start <число>, end <число>");
    Serial.println();
    Serial.println("  MONO        - монохромный цвет");
    Serial.println("    Параметры: color (RGB)");
    Serial.println("    Создание: add mono start end delay R G B");
    Serial.println("    Изменение: set <индекс> color R G B, delay <мс>, start <число>, end <число>");
    Serial.println();
    Serial.println("  DOT         - бегущая точка");
    Serial.println("    Параметры: state[0] (позиция)");
    Serial.println("    Создание: add dot start end delay [startPos]");
    Serial.println("    Изменение: set <индекс> delay <мс>, phase <знач>, start <число>, end <число>");
    Serial.println();
    Serial.println("  COMET       - комета с палитрой");
    Serial.println("    Параметры: cometStep (шаг), palette (массив цветов), state[0] (позиция головы)");
    Serial.println("    Создание: add comet start end delay [step=1] [palette=fire|ice]");
    Serial.println("    Изменение: set <индекс> step <знач>, delay <мс>, phase <знач>, start <число>, end <число>");
    Serial.println("    Палитры: fire (огненная), ice (ледяная)");
    Serial.println();
    Serial.println("  INTERPOLATE - интерполяция по точкам");
    Serial.println("    Параметры: pointCount, points[] (позиция и цвет)");
    Serial.println("    Создание: add interpolate start end delay");
    Serial.println("    Настройка: setpoints <индекс> pos R G B ...");
    Serial.println("    Изменение: set <индекс> delay <мс>, phase <знач>, start <число>, end <число>");
    Serial.println();
    Serial.println("=== Общие команды для управления слоями ===");
    Serial.println("  set <индекс> <параметр> <значение> - изменить параметр слоя");
    Serial.println("  mask <индекс> <имя_маски>          - установить маску");
    Serial.println("  phase <индекс> <значение>          - изменить фазу (в редакторе)");
    Serial.println("  remove <индекс> / delete <индекс>  - удалить слой");
    Serial.println("  list                               - показать все слои");
    Serial.println("  info                               - информация о пресете");
    Serial.println("  info full                          - подробная информация");
}

// ============================================================
//  ОБРАБОТКА КОМАНД (полная)
// ============================================================
void processCommand(String cmd) {
    cmd.trim();
    if (cmd.length() == 0) return;
    String low = cmd;
    low.toLowerCase();

    if (low == "s") {
        Serial.println("=== Команды ===");
        Serial.println("  load <имя>           - загрузить пресет/анимацию");
        Serial.println("  save <имя>           - сохранить текущие слои как статику");
        Serial.println("  list                 - показать все файлы");
        Serial.println("  deletef <имя>         - удалить файл");
        Serial.println("  info                 - информация о текущем пресете");
        Serial.println("  info full            - полная информация (кадры)");
        Serial.println("  play                 - запустить воспроизведение анимации");
        Serial.println("  stop                 - остановить воспроизведение");
        Serial.println("  pause                - пауза/продолжить");
        Serial.println("  next                 - следующий кадр");
        Serial.println("  prev                 - предыдущий кадр");
        Serial.println("  goto <номер>         - перейти к кадру");
        Serial.println("  edit                 - войти в режим редактирования");
        Serial.println("  clear                - очистить все слои");
        Serial.println("  freeze               - заморозить/разморозить движение эффектов");
        Serial.println("  se / effects         - показать список эффектов и их параметры");
        Serial.println("  l <яркость>          - установить яркость");
        Serial.println("  + / -                - изменить яркость");
        Serial.println("  o                    - выключить всё (очистить слои)");
        Serial.println("  add ...              - добавить слой");
        Serial.println("  setpoints ...        - установить точки интерполяции");
        Serial.println("  set <индекс> ...     - изменить параметры слоя");
        Serial.println("  mask ...             - установить маску");
        Serial.println("  remove <индекс>      - удалить слой");
        Serial.println("  delete <индекс>      - удалить слой (алиас remove)");
        Serial.println("  replace <индекс> ... - заменить слой (аналог add)");
        Serial.println("  rep <индекс> ...     - сокращённая версия replace");
        Serial.println("  st                   - показать текущие слои");
        Serial.println("  s                    - эта справка");
        if (editMode) {
            Serial.println("=== Команды редактора ===");
            Serial.print("Текущий кадр: ");
            Serial.println(player.currentFrame);
            Serial.print("Количество слоёв в кадре: ");
            Serial.println(player.layerCount);
            Serial.println("--- Управление кадрами ---");
            Serial.println("  nf                         - добавить новый кадр");
            Serial.println("  if <индекс>                - вставить кадр");
            Serial.println("  df <индекс>                - удалить кадр");
            Serial.println("  rf <индекс>                - заменить кадр");
            Serial.println("  sf                         - сохранить текущие слои в текущий кадр");
            Serial.println("  next / prev / goto <номер> - навигация по кадрам");
            Serial.println("--- Управление слоями в текущем кадре ---");
            Serial.println("  add <тип> ...              - добавить слой (см. se)");
            Serial.println("  insert <индекс> <тип> ...  - вставить слой перед индексом");
            Serial.println("  delete <индекс>            - удалить слой");
            Serial.println("  remove <индекс>            - удалить слой (алиас)");
            Serial.println("  replace <индекс> <тип> ... - заменить слой (аналог add)");
            Serial.println("  rep <индекс> <тип> ...      - сокращённая версия replace");
            Serial.println("  set <индекс> ...           - изменить параметры слоя");
            Serial.println("  mask <индекс> <имя>        - установить маску");
            Serial.println("--- Другие настройки ---");
            Serial.println("  change layer count <число> - изменить количество слоёв");
            Serial.println("  increase layer count <число> - увеличить количество слоёв");
            Serial.println("  framerate <число>          - изменить FPS");
            Serial.println("  type <0|1|2>               - изменить тип файла");
            Serial.println("  loop <0|1>                 - зацикливание");
            Serial.println("  delays set <индекс> <знач> - изменить задержку (var)");
            Serial.println("  delays replace <знач1> ... - заменить все задержки");
            Serial.println("  phase <слой> <значение>    - установить фазу (-1 = auto)");
            Serial.println("  save                       - сохранить изменения");
            Serial.println("  saveas <имя>               - сохранить под новым именем");
            Serial.println("  exit                       - выйти из редактора");
        }
        return;
    }

    // --- Команды управления ---
    if (low.startsWith("load ")) { String name = cmd.substring(5); name.trim(); if (name.length()) loadPreset(name.c_str()); return; }
    if (low.startsWith("save ")) { String name = cmd.substring(5); name.trim(); if (name.length()) saveStatic(name.c_str()); return; }
    if (low == "list") { listFiles(); return; }
    if (low.startsWith("deletef ")) { String name = cmd.substring(8); name.trim(); deleteFile(name.c_str()); return; }
    if (low == "info") { printInfo(false); return; }
    if (low.startsWith("info full")) { printInfo(true); return; }
    if (low == "play") { playAnimation(); return; }
    if (low == "stop") { stopAnimation(); return; }
    if (low == "pause") { pauseAnimation(); return; }
    if (low == "next") { nextFrame(); return; }
    if (low == "prev") { prevFrame(); return; }
    if (low.startsWith("goto ")) { int num = extractNumber(cmd.substring(5)); gotoFrame(num); return; }
    if (low == "edit") { enterEditMode(); return; }
    if (low == "clear") { clearLayers(); Serial.println("Все слои удалены"); return; }
    if (low == "freeze") {
        effectsPaused = !effectsPaused;
        Serial.print("Эффекты ");
        Serial.println(effectsPaused ? "заморожены" : "разморожены");
        return;
    }
    if (low == "se" || low == "effects") {
        showEffects();
        return;
    }
    if (low == "+") { brightness = min(255, brightness+5); FastLED.setBrightness(brightness); Serial.print("Яркость: "); Serial.println(brightness); return; }
    if (low == "-") { brightness = max(0, brightness-5); FastLED.setBrightness(brightness); Serial.print("Яркость: "); Serial.println(brightness); return; }
    if (low.startsWith("l ")) { int val = extractNumber(cmd.substring(2)); if (val>=0 && val<=255) { brightness=val; FastLED.setBrightness(brightness); Serial.print("Яркость: "); Serial.println(brightness); } return; }
    if (low == "o") { clearLayers(); Serial.println("Все слои удалены"); return; }

    // --- Команда st (показать слои) ---
    if (low == "st") {
        Serial.println("=== Текущие слои ===");
        for (int i=0; i<layerCount; i++) {
            Serial.print(i); Serial.print(": ");
            switch (layers[i].type) {
                case EF_NONE: Serial.print("NONE"); break;
                case EF_RAINBOW: Serial.print("RAINBOW"); break;
                case EF_MONO: Serial.print("MONO"); break;
                case EF_DOT: Serial.print("DOT"); break;
                case EF_COMET: Serial.print("COMET"); break;
                case EF_INTERPOLATE: Serial.print("INTERPOLATE"); break;
            }
            Serial.print(" [");
            Serial.print(layers[i].start);
            Serial.print("..");
            Serial.print(layers[i].end);
            Serial.print("] delay=");
            Serial.print(layers[i].delay);
            Serial.print(" state[0]=");
            Serial.print(layers[i].params.state[0]);
            Serial.print(" phase=");
            Serial.print(layers[i].params.phase);
            Serial.print(" mask=");
            Serial.println(getMaskName(&layers[i]));
        }
        return;
    }

    // --- Команда set (изменение параметров слоя) ---
    if (low.startsWith("set ")) {
        String rest = cmd.substring(4);
        rest.trim();
        int idx = extractNumber(rest);
        int pos = 0;
        while (pos < rest.length() && isDigit(rest.charAt(pos))) pos++;
        while (pos < rest.length() && rest.charAt(pos) == ' ') pos++;
        rest = rest.substring(pos);
        rest.trim();
        if (idx < 0 || idx >= layerCount) {
            Serial.println("Ошибка: неверный индекс слоя");
            return;
        }
        Layer* layer = &layers[idx];
        int space = rest.indexOf(' ');
        if (space == -1) {
            Serial.println("Ошибка: не хватает параметра и значения");
            return;
        }
        String param = rest.substring(0, space);
        String valueStr = rest.substring(space + 1);
        valueStr.trim();
        param.toLowerCase();

        if (param == "hueoffset") {
            int val = valueStr.toInt();
            if (layer->type == EF_RAINBOW) {
                layer->params.state[0] = val;
                Serial.println("Начальный оттенок (state[0]) установлен");
            } else {
                Serial.println("Этот слой не является радугой");
            }
        } else if (param == "speedstep") {
            int val = valueStr.toInt();
            if (layer->type == EF_RAINBOW) {
                layer->params.speedStep = (uint8_t)val;
                Serial.println("speedStep обновлён");
            } else {
                Serial.println("Этот слой не является радугой");
            }
        } else if (param == "huestep") {
            int val = valueStr.toInt();
            if (layer->type == EF_RAINBOW) {
                layer->params.hueStep = (uint8_t)val;
                Serial.println("hueStep обновлён");
            } else {
                Serial.println("Этот слой не является радугой");
            }
        } else if (param == "step") {
            int val = valueStr.toInt();
            if (layer->type == EF_COMET) {
                layer->params.cometStep = val;
                Serial.println("Шаг кометы обновлён");
            } else {
                Serial.println("Этот слой не является кометой");
            }
        } else if (param == "color") {
            CRGB color;
            if (parseRGB(valueStr, color)) {
                if (layer->type == EF_MONO) {
                    layer->params.color = color;
                    Serial.println("Цвет обновлён");
                } else {
                    Serial.println("Этот слой не является монохромным");
                }
            } else {
                Serial.println("Ошибка формата RGB");
            }
        } else if (param == "delay") {
            int val = valueStr.toInt();
            if (val >= 0) {
                layer->delay = val;
                Serial.println("Задержка обновлена");
            } else {
                Serial.println("Ошибка: задержка должна быть >= 0");
            }
        } else if (param == "phase") {
            int val = valueStr.toInt();
            if (val == -1) {
                layer->params.phase = 0xFFFFFFFF;
                Serial.println("Фаза установлена в auto");
            } else if (val >= 0) {
                layer->params.phase = (uint32_t)val;
                Serial.println("Фаза установлена");
            } else {
                Serial.println("Ошибка: фаза должна быть -1 (auto) или >= 0");
            }
        } else if (param == "start") {
            int val = valueStr.toInt();
            if (val >= 0 && val < NUM_LEDS && val <= layer->end) {
                layer->start = val;
                Serial.println("start обновлён");
            } else {
                Serial.println("Ошибка: неверное значение start (должно быть 0..NUM_LEDS-1 и <= end)");
            }
        } else if (param == "end") {
            int val = valueStr.toInt();
            if (val >= 0 && val < NUM_LEDS && val >= layer->start) {
                layer->end = val;
                Serial.println("end обновлён");
            } else {
                Serial.println("Ошибка: неверное значение end (должно быть >= start и < NUM_LEDS)");
            }
        } else {
            Serial.println("Неизвестный параметр. Доступные: hueoffset, speedstep, huestep, step, color, delay, phase, start, end");
        }
        return;
    }

    // --- Команда replace / rep (замена слоя) ---
    if (low.startsWith("replace ") || low.startsWith("rep ")) {
        String rest = cmd.substring(low.startsWith("replace ") ? 8 : 4);
        rest.trim();
        int spaceIdx = rest.indexOf(' ');
        if (spaceIdx == -1) {
            Serial.println("Ошибка: требуется индекс и параметры слоя");
            return;
        }
        String idxStr = rest.substring(0, spaceIdx);
        int idx = idxStr.toInt();
        rest = rest.substring(spaceIdx + 1);
        rest.trim();

        if (idx < 0 || idx >= layerCount) {
            Serial.println("Ошибка: неверный индекс слоя");
            return;
        }

        int space1 = rest.indexOf(' ');
        if (space1 == -1) {
            Serial.println("Ошибка: недостаточно параметров");
            return;
        }
        String typeStr = rest.substring(0, space1);
        rest = rest.substring(space1 + 1);
        rest.trim();

        int vals[3];
        if (!parseNumbers(rest, vals, 3)) {
            Serial.println("Ошибка: требуется start, end, delay");
            return;
        }
        int start = vals[0], end = vals[1], delay = vals[2];
        int pos = 0;
        for (int i = 0; i < 3; i++) {
            while (pos < rest.length() && rest.charAt(pos) == ' ') pos++;
            while (pos < rest.length() && isDigit(rest.charAt(pos))) pos++;
            while (pos < rest.length() && rest.charAt(pos) == ' ') pos++;
        }
        String paramsStr = rest.substring(pos);
        paramsStr.trim();

        EffectParams p;
        memset(&p, 0, sizeof(p));
        p.phase = 0xFFFFFFFF;

        EffectType type = getTypeFromString(typeStr);
        if (type == EF_NONE) {
            Serial.println("Неизвестный тип эффекта");
            return;
        }

        if (type == EF_RAINBOW) {
            int tmp[3] = {0, 1, 1};
            if (paramsStr.length() > 0) {
                int arr[3];
                if (parseNumbers(paramsStr, arr, 3)) {
                    for (int i = 0; i < 3; i++) tmp[i] = arr[i];
                }
            }
            p.hueStep = tmp[1];
            p.speedStep = tmp[2];
            p.state[0] = tmp[0];
        } else if (type == EF_MONO) {
            CRGB color = CRGB::Black;
            if (paramsStr.length() > 0) {
                if (!parseRGB(paramsStr, color)) {
                    Serial.println("Ошибка: неверный формат RGB");
                    return;
                }
            }
            p.color = color;
        } else if (type == EF_DOT) {
            int startPos = start;
            if (paramsStr.length() > 0) {
                int val = extractNumber(paramsStr);
                if (val >= start && val <= end) startPos = val;
            }
            p.state[0] = startPos;
        } else if (type == EF_COMET) {
            int step = 1;
            String palName = "fire";
            if (paramsStr.length() > 0) {
                int num = extractNumber(paramsStr);
                if (num > 0) step = num;
                int idx2 = 0;
                while (idx2 < paramsStr.length() && isDigit(paramsStr.charAt(idx2))) idx2++;
                while (idx2 < paramsStr.length() && paramsStr.charAt(idx2) == ' ') idx2++;
                if (idx2 < paramsStr.length()) {
                    palName = paramsStr.substring(idx2);
                    palName.trim();
                }
            }
            CRGB* pal = firePalette;
            int palSize = FIRE_PALETTE_SIZE;
            if (palName == "ice") {
                pal = icePalette;
                palSize = ICE_PALETTE_SIZE;
            }
            p.cometStep = step;
            p.paletteSize = palSize;
            for (int i = 0; i < palSize; i++) p.palette[i] = pal[i];
            p.state[0] = start;
        } else if (type == EF_INTERPOLATE) {
            p.pointCount = 0;
        }

        removeLayer(idx);
        if (idx > layerCount) idx = layerCount;
        int newIdx = insertLayer(idx, type, start, end, delay, p, nullptr);
        if (newIdx != -1) {
            Serial.print("Слой заменён на позиции ");
            Serial.println(idx);
        } else {
            Serial.println("Ошибка при замене слоя");
        }
        return;
    }

    // --- Команды add, setpoints, mask ---
    if (low.startsWith("add ")) {
        String rest = cmd.substring(4);
        rest.trim();
        int space1 = rest.indexOf(' ');
        if (space1 == -1) {
            Serial.println("Ошибка: недостаточно параметров");
            return;
        }
        String typeStr = rest.substring(0, space1);
        rest = rest.substring(space1 + 1);
        rest.trim();

        int vals[3];
        if (!parseNumbers(rest, vals, 3)) {
            Serial.println("Ошибка: требуется start, end, delay");
            return;
        }
        int start = vals[0], end = vals[1], delay = vals[2];
        int pos = 0;
        for (int i = 0; i < 3; i++) {
            while (pos < rest.length() && rest.charAt(pos) == ' ') pos++;
            while (pos < rest.length() && isDigit(rest.charAt(pos))) pos++;
            while (pos < rest.length() && rest.charAt(pos) == ' ') pos++;
        }
        String paramsStr = rest.substring(pos);
        paramsStr.trim();

        EffectParams p;
        memset(&p, 0, sizeof(p));
        p.phase = 0xFFFFFFFF;

        if (typeStr == "rainbow") {
            int tmp[3] = {0, 1, 1};
            if (paramsStr.length() > 0) {
                int arr[3];
                if (parseNumbers(paramsStr, arr, 3)) {
                    for (int i = 0; i < 3; i++) tmp[i] = arr[i];
                }
            }
            p.hueStep = tmp[1];
            p.speedStep = tmp[2];
            p.state[0] = tmp[0];
            addLayer(EF_RAINBOW, start, end, delay, p);
            Serial.println("Добавлен RAINBOW");
        }
        else if (typeStr == "mono") {
            CRGB color = CRGB::Black;
            if (paramsStr.length() > 0) {
                if (!parseRGB(paramsStr, color)) {
                    Serial.println("Ошибка: неверный формат RGB");
                    return;
                }
            }
            p.color = color;
            addLayer(EF_MONO, start, end, delay, p);
            Serial.println("Добавлен MONO");
        }
        else if (typeStr == "dot") {
            int startPos = start;
            if (paramsStr.length() > 0) {
                int val = extractNumber(paramsStr);
                if (val >= start && val <= end) startPos = val;
            }
            p.state[0] = startPos;
            addLayer(EF_DOT, start, end, delay, p);
            Serial.println("Добавлен DOT");
        }
        else if (typeStr == "comet") {
            int step = 1;
            String palName = "fire";
            if (paramsStr.length() > 0) {
                int num = extractNumber(paramsStr);
                if (num > 0) step = num;
                int idx2 = 0;
                while (idx2 < paramsStr.length() && isDigit(paramsStr.charAt(idx2))) idx2++;
                while (idx2 < paramsStr.length() && paramsStr.charAt(idx2) == ' ') idx2++;
                if (idx2 < paramsStr.length()) {
                    palName = paramsStr.substring(idx2);
                    palName.trim();
                }
            }
            CRGB* pal = firePalette;
            int palSize = FIRE_PALETTE_SIZE;
            if (palName == "ice") {
                pal = icePalette;
                palSize = ICE_PALETTE_SIZE;
            }
            p.cometStep = step;
            p.paletteSize = palSize;
            for (int i = 0; i < palSize; i++) p.palette[i] = pal[i];
            p.state[0] = start;
            addLayer(EF_COMET, start, end, delay, p);
            Serial.println("Добавлен COMET");
        }
        else if (typeStr == "interpolate") {
            p.pointCount = 0;
            addLayer(EF_INTERPOLATE, start, end, delay, p);
            Serial.println("Добавлен INTERPOLATE (без точек)");
        }
        else {
            Serial.println("Неизвестный тип эффекта");
        }
        return;
    }

    if (low.startsWith("setpoints ")) {
        String rest = cmd.substring(10);
        rest.trim();
        int idx = extractNumber(rest);
        int pos = 0;
        while (pos < rest.length() && isDigit(rest.charAt(pos))) pos++;
        while (pos < rest.length() && rest.charAt(pos) == ' ') pos++;
        rest = rest.substring(pos);
        rest.trim();

        if (idx < 0 || idx >= layerCount) {
            Serial.println("Ошибка: неверный индекс слоя");
            return;
        }
        Layer* layer = &layers[idx];
        if (layer->type != EF_INTERPOLATE) {
            Serial.println("Ошибка: слой не является интерполяцией");
            return;
        }

        layer->params.pointCount = 0;
        while (rest.length() > 0 && layer->params.pointCount < MAX_POINTS) {
            while (rest.length() > 0 && rest.charAt(0) == ' ') rest = rest.substring(1);
            if (rest.length() == 0) break;

            int valPos = 0;
            bool hasPos = false;
            while (rest.length() > 0 && isDigit(rest.charAt(0))) {
                valPos = valPos * 10 + (rest.charAt(0) - '0');
                rest = rest.substring(1);
                hasPos = true;
            }
            if (!hasPos) break;
            while (rest.length() > 0 && rest.charAt(0) == ' ') rest = rest.substring(1);

            int rgb[3];
            bool ok = true;
            for (int i = 0; i < 3; i++) {
                int val = 0;
                bool hasDigit = false;
                while (rest.length() > 0 && isDigit(rest.charAt(0))) {
                    val = val * 10 + (rest.charAt(0) - '0');
                    rest = rest.substring(1);
                    hasDigit = true;
                }
                if (!hasDigit) { ok = false; break; }
                rgb[i] = val;
                while (rest.length() > 0 && rest.charAt(0) == ' ') rest = rest.substring(1);
            }
            if (!ok) {
                Serial.println("Ошибка: неверный формат точки");
                return;
            }
            if (valPos < 0 || valPos >= NUM_LEDS) {
                Serial.println("Ошибка: позиция вне диапазона");
                return;
            }
            if (rgb[0] < 0 || rgb[0] > 255 || rgb[1] < 0 || rgb[1] > 255 || rgb[2] < 0 || rgb[2] > 255) {
                Serial.println("Ошибка: цвет должен быть 0-255");
                return;
            }
            int idxPoint = layer->params.pointCount;
            layer->params.points[idxPoint].pos = valPos;
            layer->params.points[idxPoint].color = CRGB(rgb[0], rgb[1], rgb[2]);
            layer->params.pointCount++;
        }
        Serial.print("Установлено точек: ");
        Serial.println(layer->params.pointCount);
        return;
    }

    if (low.startsWith("mask ")) {
        String rest = cmd.substring(5);
        rest.trim();
        int idx = extractNumber(rest);
        int pos = 0;
        while (pos < rest.length() && isDigit(rest.charAt(pos))) pos++;
        while (pos < rest.length() && rest.charAt(pos) == ' ') pos++;
        String maskName = rest.substring(pos);
        maskName.trim();
        if (idx < 0 || idx >= layerCount) { Serial.println("Неверный индекс"); return; }
        bool (*maskFunc)(int) = getMaskByName(maskName.c_str());
        layers[idx].mask = maskFunc;
        Serial.print("Маска для слоя "); Serial.print(idx); Serial.println(" установлена");
        return;
    }

    // --- Команды редактора ---
    if (editMode) {
        // Навигация по кадрам
        if (low == "next") { nextFrame(); return; }
        if (low == "prev") { prevFrame(); return; }
        if (low.startsWith("goto ")) {
            int num = extractNumber(cmd.substring(5));
            gotoFrame(num);
            return;
        }
        // Зацикливание
        if (low.startsWith("loop ")) {
            int val = extractNumber(cmd.substring(5));
            if (val == 0 || val == 1) {
                player.loop = val;
                editDirty = true;
                Serial.print("Зацикливание установлено: ");
                Serial.println(val ? "ВКЛ" : "ВЫКЛ");
            } else {
                Serial.println("Ошибка: используйте 0 (выкл) или 1 (вкл)");
            }
            return;
        }
        // Работа с кадрами
        if (low == "nf") { addFrame(); return; }
        if (low.startsWith("if ")) { int idx = extractNumber(cmd.substring(3)); insertFrame(idx); return; }
        if (low.startsWith("df ")) { int idx = extractNumber(cmd.substring(3)); deleteFrame(idx); return; }
        if (low.startsWith("rf ")) { int idx = extractNumber(cmd.substring(3)); replaceFrame(idx); return; }
        // Сохранить текущие слои в текущий кадр
        if (low == "sf") {
            if (player.type == 0) {
                Serial.println("Это статика, сохранение в кадр невозможно");
                return;
            }
            if (player.frameCount == 0) {
                Serial.println("Нет кадров для сохранения");
                return;
            }
            replaceFrame(player.currentFrame);
            Serial.println("Кадр сохранён");
            return;
        }
        // Сохранение
        if (low == "save") {
            saveAnimation(player.fileName);
            editDirty = false;
            return;
        }
        if (low.startsWith("saveas ")) {
            String name = cmd.substring(7);
            name.trim();
            if (name.length() > 0) {
                saveAnimation(name.c_str());
                strncpy(player.fileName, name.c_str(), MAX_PRESET_NAME-1);
                player.fileName[MAX_PRESET_NAME-1] = '\0';
                editDirty = false;
            }
            return;
        }
        // Остальные команды редактора
        if (low.startsWith("change layer count ")) {
            int count = extractNumber(cmd.substring(19));
            changeLayerCount(count);
            return;
        }
        if (low.startsWith("increase layer count ")) {
            int inc = extractNumber(cmd.substring(21));
            changeLayerCount(player.layerCount + inc);
            return;
        }
        if (low.startsWith("framerate ")) {
            int fps = extractNumber(cmd.substring(10));
            setFPS(fps);
            return;
        }
        if (low.startsWith("type ")) {
            int t = extractNumber(cmd.substring(5));
            changeType(t);
            return;
        }
        if (low.startsWith("delays set ")) {
            String rest = cmd.substring(11);
            rest.trim();
            int space = rest.indexOf(' ');
            if (space != -1) {
                int idx = rest.substring(0, space).toInt();
                int val = rest.substring(space+1).toInt();
                setDelay(idx, val);
            }
            return;
        }
        if (low.startsWith("delays replace ")) {
            String values = cmd.substring(14);
            values.trim();
            replaceDelays(values);
            return;
        }
        if (low.startsWith("phase ")) {
            String rest = cmd.substring(6);
            rest.trim();
            int space = rest.indexOf(' ');
            if (space != -1) {
                int layerIdx = rest.substring(0, space).toInt();
                int phase = rest.substring(space+1).toInt();
                setPhase(layerIdx, phase);
            }
            return;
        }
        if (low == "exit") {
            exitEditMode();
            return;
        }
        // Команды удаления слоёв в редакторе
        if (low.startsWith("delete ")) {
            int idx = extractNumber(cmd.substring(7));
            removeLayer(idx);
            Serial.println("Слой удалён");
            return;
        }
        // Если команда не распознана
        Serial.println("Неизвестная команда редактора. Введите s для справки.");
        return;
    }

    // Команда delete вне редактора (удаление слоя) — чтобы не было конфликта с deletef для файлов
    if (low.startsWith("delete ")) {
        int idx = extractNumber(cmd.substring(7));
        if (idx >= 0 && idx < layerCount) {
            removeLayer(idx);
            Serial.println("Слой удалён");
        } else {
            Serial.println("Неверный индекс слоя");
        }
        return;
    }

    Serial.println("Неизвестная команда. Введите s для справки.");
}

// ============================================================
//  SETUP И LOOP
// ============================================================
void setup() {
    Serial.begin(115200);
    Serial.println("=== LED-контроллер версии 2.8 ===");

    FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
    FastLED.setBrightness(brightness);
    FastLED.clear();
    FastLED.show();

    if (!initFS()) {
        Serial.println("Ошибка инициализации LittleFS.");
    }

    memset(&player, 0, sizeof(player));
    editMode = false;
    effectsPaused = false;

    Serial.println("Введите s для справки.");
}

void loop() {
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        if (cmd.length() > 0) {
            processCommand(cmd);
        }
    }

    updateAnimation();

    fill_solid(leds, NUM_LEDS, CRGB::Black);
    for (int i = 0; i < layerCount; i++) {
        if (layers[i].draw) {
            layers[i].draw(&layers[i], leds);
        }
    }
    FastLED.show();
    delay(10);
}