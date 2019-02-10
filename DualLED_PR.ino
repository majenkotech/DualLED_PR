#include <LIBus.h>
#include <TLC59116_DTWI.h>
#include <DebouncedInput.h>
#include <EEPROM.h>

DTWI0 dtwi;
TLC59116_DTWI board1(dtwi, 0);

DebouncedInput trigger(0, 20, true);
DebouncedInput magazine(1, 20, true);
DebouncedInput modeswitch(2, 20, true);
DebouncedInput profileswitch(3, 20, true);

struct globalSettings {
    uint16_t id; // 0x5250 (PR in little-endian)
    uint8_t brightness;
    uint8_t defprofile;
    uint8_t numprofiles;
};

struct profileSettings {
    uint16_t id; 
    uint8_t init;
    uint8_t ammo;
    uint8_t speed1;
    uint8_t speed2;
    uint8_t defmode;
    uint8_t direction;
    uint8_t magfunc;
};

USBFS usbDevice;
USBManager USB(usbDevice, 0xf055, 0x041A, "Majenko Technologies", "M41A Pulse Rifle");
CDCACM uSerial;

uint8_t ammo = 0;

enum {
    IDLE,
    LOADING,
    ACTIVE,
    BLOCKED
};

#define MAX_PROFILES 8
#define PROFILE_START 0x08
#define PROFILE_SPACING 0x10

struct globalSettings global;
struct profileSettings profiles[MAX_PROFILES];

uint8_t selectedProfile = 0;

uint8_t trigState = IDLE;
uint8_t magState = IDLE;
uint8_t ammoState = IDLE;

static inline void showState(uint8_t state) {
    return;
    switch(state) {
        case IDLE:             uSerial.println((const char *)"IDLE"); break;
        case LOADING:          uSerial.println((const char *)"LOADING"); break;
        case ACTIVE:           uSerial.println((const char *)"ACTIVE"); break;
        case BLOCKED:          uSerial.println((const char *)"BLOCKED"); break;
    }
}

static inline void setAmmoState(uint8_t state) {
    ammoState = state;
    uSerial.print((const char *)"A:");
    showState(ammoState);
}

static inline void setMagState(uint8_t state) {
    magState = state;
    uSerial.print((const char *)"M:");
    showState(magState);
}

static inline void setTrigState(uint8_t state) {
    trigState = state;
    uSerial.print((const char *)"T:");
    showState(trigState);
}

const uint8_t pinmap[8] = {
    0x2 << 4 | 0x5,
    0x3 << 4 | 0x6,
    0xd << 4 | 0x8,
    0xe << 4 | 0xa,
    0xf << 4 | 0xb,
    0x0 << 4 | 0x4,
    0x1 << 4 | 0x9,
    0xc << 4 | 0x7
};

void displaySettings(struct profileSettings *prof) {
    char temp[5];
    uSerial.print("\x1b[2J\x1b[1;1H");
    uSerial.println("uPR Settings");
    uSerial.println("============");
    uSerial.println();
    uSerial.printf("[B/b] Brightness: %u\r\n", global.brightness);
    uSerial.printf("[P/p] Profiles:   %u\r\n", global.numprofiles);

    uSerial.println();

    uSerial.printf("Profile: %d\r\n", selectedProfile);
    uSerial.println("===========");
    uSerial.printf("[L/l] Load speed:          %3ums\r\n", prof->speed1);
    uSerial.printf("[F/f] Fire speed:          %3ums\r\n", prof->speed2);
    uSerial.printf("[I/i] Initial ammo:        %2u\r\n", prof->init);
    uSerial.printf("[A/a] Loaded ammo:         %2u\r\n", prof->ammo);
    uSerial.printf("[E/e] Default mode:        %s\r\n", prof->defmode == 1 ? "single" : "auto");  
    uSerial.printf("[D/d] Count direction:     %s\r\n", prof->direction == 1 ? "up" : "down"); 
    uSerial.printf("[M/m] MAG button function: %s\r\n", prof->magfunc == 0 ? "magazine" : "ammo reset");  

    uSerial.println();
    uSerial.println("S: Save R: Reset N: Next profile n: Prev profile");

    uSerial.println();
    uSerial.println("Capitals increase, lower-case decreases.");
}

uint8_t tweak(uint8_t *val, uint8_t min, uint8_t max, int8_t dir) {
    if ((dir == -1) && (*val <= min)) {
        *val = min;
        return 0;
    }
    if ((dir == 1) && (*val >= max)) {
        *val = max;
        return 0;
    }

    *val += dir;
    return 1;
}

void uSerialMenu(struct profileSettings *prof) {
    static uint32_t saveTimer = 0;

    if (uSerial.available()) {
        int sr = uSerial.read();

        switch(sr) {
            case 'B': tweak(&global.brightness, 0, 7, +1); break;
            case 'b': tweak(&global.brightness, 0, 7, -1); break;
            case 'P': tweak(&global.numprofiles, 0, MAX_PROFILES-1, +1); break;
            case 'p': tweak(&global.numprofiles, 0, MAX_PROFILES-1, -1); break;
            case 'L': tweak(&prof->speed1, 0, 255, +1); break;
            case 'l': tweak(&prof->speed1, 0, 255, -1); break;
            case 'F': tweak(&prof->speed2, 0, 255, +1); break;
            case 'f': tweak(&prof->speed2, 0, 255, -1); break;
            case 'I': tweak(&prof->init, 0, 99, +1); break;
            case 'i': tweak(&prof->init, 0, 99, -1); break;
            case 'A': tweak(&prof->ammo, 0, 99, +1); break;
            case 'a': tweak(&prof->ammo, 0, 99, -1); break;
            case 'D': tweak(&prof->direction, 0, 1, +1); break;
            case 'd': tweak(&prof->direction, 0, 1, -1); break;
            case 'E': tweak(&prof->defmode, 0, 1, +1); break;
            case 'e': tweak(&prof->defmode, 0, 1, -1); break;
            case 'M': tweak(&prof->magfunc, 0, 1, +1); break;
            case 'm': tweak(&prof->magfunc, 0, 1, -1); break;
            case 'n': if (selectedProfile > 0) selectedProfile--; prof = &profiles[selectedProfile]; break;
            case 'N': if (selectedProfile < global.numprofiles-1) selectedProfile++; &profiles[selectedProfile]; break;
            case 'r':
                executeSoftReset(0);
                break;
            case 's':
                saveGlobalSettings();
                for (int i = 0; i < MAX_PROFILES; i++) {
                    saveProfileSettings(i);
                }
                uSerial.println("All saved.");
                delay(1000);
                break;
        }
        displaySettings(prof);
    }
}

void setAmmo(uint8_t val) {
    ammo = val;
    board1.displayNumber(val, 1<<global.brightness);
}

void saveGlobalSettings() {
    uint8_t *d = (uint8_t *)&global;
    for (int i = 0; i < sizeof(struct globalSettings); i++) {
        EEPROM.write(i, d[i]);
    }    
}

void saveProfileSettings(int prof) {
    uint8_t *d = (uint8_t *)&profiles[prof];
    for (int i = 0; i < sizeof(struct profileSettings); i++) {
        EEPROM.write(PROFILE_START + (prof * PROFILE_SPACING) + i, d[i]);
    }
}

void loadEEPROMData() {
    uint8_t *d = (uint8_t *)&global;
    for (int i = 0; i < sizeof(struct globalSettings); i++) {
        d[i] = EEPROM.read(i);
    }

    if (global.id != 0x5250) {
        global.id = 0x5250;
        global.brightness = 7;
        global.defprofile = 0; 
        global.numprofiles = 1;
        saveGlobalSettings();
    }

    for (int prof = 0; prof < MAX_PROFILES; prof++) {
        d = (uint8_t *)&profiles[prof];
        for (int i = 0; i < sizeof(struct profileSettings); i++) {
            d[i] = EEPROM.read(PROFILE_START + (prof * PROFILE_SPACING) + i);
        }
        if (profiles[prof].id != 0x55AA) {
            profiles[prof].id = 0x55AA;
            profiles[prof].init = 99;
            profiles[prof].ammo = 95;
            profiles[prof].speed1 = 250;
            profiles[prof].speed2 = 66;
            profiles[prof].defmode = 0;
            profiles[prof].direction = 0;
            profiles[prof].magfunc = 0;
            saveProfileSettings(prof);
        }
    }
}

void setup() {
    USB.addDevice(uSerial);
    USB.begin();


    // Short delay to allow the TLC59116 to boot up.
    // Normally the bootloader takes care of this, but
    // when not delaying with no USB connected there
    // seems to be a need to wait a moment for things
    // to settle down.
    delay(100);
    
	board1.begin();
    board1.setPinMapping(pinmap);
    board1.setLeadingZero(true);

    trigger.begin();
    magazine.begin();
    modeswitch.begin();
    profileswitch.begin();
    
    for (int i = 0; i < 16; i++) {
        board1.analogWrite(i, 0);
    }

    loadEEPROMData();
    selectedProfile = global.defprofile;
    
    if (magazine.read() == LOW) {
        setAmmo(profiles[selectedProfile].init);
        setAmmoState(LOADING);
        setMagState(ACTIVE);
    } else {
        setAmmo(0);
        setAmmoState(IDLE);
        setMagState(IDLE);
    }

    if (profiles[selectedProfile].magfunc == 1) { // Ammo reset function
        setAmmo(profiles[selectedProfile].init);
        setAmmoState(LOADING);
    }
}

void displayAuto(uint8_t bright) {
    board1.analogWrite(0, bright);  // f1
    board1.analogWrite(1, bright);  // g1
    board1.analogWrite(2, bright);  // a1
    board1.analogWrite(3, bright);  // b1
    board1.analogWrite(4, 0);       // f2
    board1.analogWrite(5, 0);       // a2
    board1.analogWrite(6, 0);       // b2
    board1.analogWrite(7, 0);       // dp2
    board1.analogWrite(8, bright);  // c2
    board1.analogWrite(9, 0);       // g2
    board1.analogWrite(10, bright); // d2
    board1.analogWrite(11, bright); // e2
    board1.analogWrite(12, 0);      // dp1
    board1.analogWrite(13, bright); // c1
    board1.analogWrite(14, 0);      // d1
    board1.analogWrite(15, bright); // e1
}

void displaySingle(uint8_t bright) {
    board1.analogWrite(0, bright);  // f1
    board1.analogWrite(1, bright);  // g1
    board1.analogWrite(2, bright);  // a1
    board1.analogWrite(3, 0);       // b1
    board1.analogWrite(4, 0);       // f2
    board1.analogWrite(5, 0);       // a2
    board1.analogWrite(6, 0);       // b2
    board1.analogWrite(7, 0);       // dp2
    board1.analogWrite(8, 0);       // c2
    board1.analogWrite(9, 0);       // g2
    board1.analogWrite(10, 0);      // d2
    board1.analogWrite(11, bright); // e2
    board1.analogWrite(12, 0);      // dp1
    board1.analogWrite(13, bright); // c1
    board1.analogWrite(14, bright); // d1
    board1.analogWrite(15, 0);      // e1
}

void displayP(uint8_t p, uint8_t bright) {
    board1.displayNumber(p, bright);
    board1.analogWrite(2, bright);  // a1
    board1.analogWrite(3, bright);  // b1
    board1.analogWrite(13, 0);      // c1
    board1.analogWrite(14, 0);      // d1
    board1.analogWrite(15, bright); // e1
    board1.analogWrite(0, bright);  // f1
    board1.analogWrite(1, bright);  // g1
    board1.analogWrite(12, 0);      // dp1
}


void loop() {
    static uint8_t state = LOADING;
    static uint32_t ts = 0;
    static uint32_t triggerWentLow = 0;
    static uint8_t mode = mode = profiles[selectedProfile].defmode;;
    static uint8_t modeSwitchValue = HIGH;
    static uint32_t modeTs = 0;
    
    delay(1);

    uSerialMenu(&profiles[selectedProfile]);

    if ((modeTs > 0) && ((millis() - modeTs) >= 1000)) {
        setAmmo(ammo);
        modeTs = 0;
    }

    if (modeswitch.changedTo(LOW)) {
        mode = 1 - mode;
        modeTs = millis();
        if (mode == 0) {
            displayAuto(1 << global.brightness);
        } else {
            displaySingle(1 << global.brightness);
        }
    }

    if (profileswitch.changedTo(LOW)) {
        selectedProfile++;
        if (selectedProfile >= global.numprofiles) {
            selectedProfile = 0;
        }
        displayP(selectedProfile, 1 << global.brightness);
        mode = profiles[selectedProfile].defmode;
        global.defprofile = selectedProfile;
        saveGlobalSettings();
    }

    if (profiles[selectedProfile].magfunc == 0) { // Normal magazine operation       
        if (magazine.read()== LOW && magState == IDLE) { // Inserted
            setAmmoState(LOADING);
            setAmmo(profiles[selectedProfile].init);
            setMagState(ACTIVE);
            ts = millis();
        }

        if (magazine.read() == HIGH && magState == ACTIVE) { // Removed
            setAmmoState(IDLE);
            setAmmo(0);
            setMagState(IDLE);
        }
    } else if (profiles[selectedProfile].magfunc == 1) { // Simple ammo reset
        if (magazine.read() == LOW && magState == IDLE) { // Pressed
            setAmmoState(LOADING);
            setAmmo(profiles[selectedProfile].init);
        }

        if (magazine.read() == HIGH && magState == ACTIVE) { // Released
            // Nothing is done here.
        }
    }

    if (trigger.read() == LOW && trigState == IDLE) {
        setTrigState(ACTIVE);
        ts = millis();
    }

    if (trigger.read() == HIGH && trigState == ACTIVE) {
        setTrigState(IDLE);
    }

    if (trigger.read() == HIGH && trigState == BLOCKED) {
        setTrigState(IDLE);
    }


    if (ammoState == LOADING) {
        if (millis() - ts > profiles[selectedProfile].speed1) {
            ts = millis();

            if (ammo == profiles[selectedProfile].ammo) {
                setAmmoState(ACTIVE);
            } else {
                setAmmo(ammo-1);
            }
        }
    }

    if (ammoState == ACTIVE) {
        if (trigState == ACTIVE) {
            if (mode == 1) {
                setTrigState(BLOCKED);
                setAmmo(ammo-1);
    
                if (ammo == 0) {
                    setAmmoState(IDLE);
                }                
            } else {
                if (millis() - ts > profiles[selectedProfile].speed2) {
                //    digitalWrite(fpulse, HIGH);
                    ts = millis();
                    setAmmo(ammo-1);
    
                    if (ammo == 0) {
                        setAmmoState(IDLE);
                    }                
                }
            }
        }
    }
    
}
