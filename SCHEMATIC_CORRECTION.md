# Schematic Correction Notice

## Issue with Original Schematic

The original schematic from the Printables design shows:
- **TP4056 OUT+** (red wire) → **3V3 pin** on FireBeetle 2 ESP32-E

## This is INCORRECT and can damage your ESP32!

### Why it's wrong:
- **3V3** is a regulated 3.3V **OUTPUT** pin
- **TP4056 OUT+** outputs battery voltage (~4V) or USB voltage (~5V)
- Connecting 4-5V to a 3.3V pin can permanently damage the ESP32

### Correct Connection:
- **TP4056 OUT+** (red wire) → **VCC pin** on FireBeetle 2 ESP32-E
- OR use the **PH2.0 battery connector** on the FireBeetle (recommended)

### Visual Correction:

**WRONG (from original schematic):**
```
TP4056 OUT+ ───> 3V3 pin  ❌ DANGEROUS!
```

**CORRECT:**
```
TP4056 OUT+ ───> VCC pin  ✅ SAFE!
```

The FireBeetle's onboard voltage regulator will safely convert the VCC input (3.7-4.2V from battery) to 3.3V internally.

## Other Connections Remain the Same

All other connections in the original schematic are correct:
- TP4056 OUT- → GND
- TP4056 B+ → Battery +
- TP4056 B- → Battery -
- Switches → GPIO pins (as shown)
- All GND connections remain the same

## Updated Wiring Summary

```
TP4056 Charging Board          FireBeetle 2 ESP32-E
─────────────────────          ─────────────────────
OUT+ (red wire)    ──────────> VCC ✅ (NOT 3V3!)
OUT- (black wire)  ──────────> GND
B+                 ──────────> 18650 Battery +
B-                 ──────────> 18650 Battery -
```

