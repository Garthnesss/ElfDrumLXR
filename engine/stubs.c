/* No-op stand-ins for the front panel, LEDs, menu, UART, USB and trigger outputs: the host has none of them.
   Copied from mechanical-dance/PHX-D01 host/stubs.c (GPL-3.0; see NOTICE). */
#include "stm32f7xx_hal.h"
#include "Soundengine/MIDI/MidiMessages.h"
#include "Preset/PresetManager.h"
#include "menu/menu.h"

GPIO_TypeDef native_gpio[11];
RNG_HandleTypeDef hrng;
volatile uint32_t systick_ticks;
uint8_t activeParameters[NUM_PARAMS];
uint8_t menu_activeBar, menu_activePage, menu_playedPattern, menu_shownPattern, menu_songMenuPosition;
uint8_t menu_loadedPresetNr[NUM_CURRENT];
uint8_t menu_selectedPresetNr[NUM_PRESET_LOCATIONS];
uint8_t midi_process_block;
uint8_t trigger_clockReceived, trigger_dividerClockOut1, trigger_prescalerClockInput;

float lxr_fader_gain[6] = {1.f, 1.f, 1.f, 1.f, 1.f, 1.f};   /* set by lxr_set_fader */
float adc_getFaderVolume(uint8_t idx) { return idx < 6 ? lxr_fader_gain[idx] : 1.f; }
uint8_t buttonHandler_getMode() { return 0; }
void led_blinkHandler() {}
void led_clearActive_step() {}
void led_clearAllBlinkLeds() {}
void led_clearSequencerLeds() {}
uint8_t led_idx2StepLed(uint8_t idx) { return idx; }
void led_initPatternSelectLeds() {}
void led_resetBlinkTimer() {}
void led_setActive_step(uint8_t stepNr) { (void)stepNr; }
void led_setBlinkLed(const uint8_t ledNr, const uint8_t onOff) { (void)ledNr; (void)onOff; }
void led_setValue(uint8_t val, uint8_t ledNr) { (void)val; (void)ledNr; }
void led_updateSeqLeds() {}
uint8_t menu_getActiveVoice() { return 0; }
uint8_t menu_getSubPage() { return 0; }
uint8_t menu_getViewedPattern() { return 0; }
void menu_repaint() {}
void menu_repaintAll() {}
void menu_setShownPattern(uint8_t patternNr) { (void)patternNr; }
void menu_switchSubPage(uint8_t subPageNr, uint8_t toggle) { (void)subPageNr; (void)toggle; }
void preset_loadDrumsetRam(uint8_t presetNr, uint8_t isMorph, uint8_t sendImmediate) { (void)presetNr; (void)isMorph; (void)sendImmediate; }
void preset_touchTimer(PresetTimer type) { (void)type; }
void trigger_clockTick(uint8_t onOff, uint8_t cnt) { (void)onOff; (void)cnt; }
void trigger_reset(uint8_t value) { (void)value; }
void uart_clearFrontFifo() {}
void uart_sendFrontpanelByte(uint8_t data) { (void)data; }
void uart_sendFrontpanelSysExByte(uint8_t data) { (void)data; }
void uart_sendMidi(MidiMsg msg) { (void)msg; }
void usb_sendMidi(MidiMsg msg) { (void)msg; }
