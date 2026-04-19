#include "wled.h"

// for information how FX metadata strings work see https://kno.wled.ge/interfaces/json-api/#effect-metadata

// paletteBlend: 0 - wrap when moving, 1 - always wrap, 2 - never wrap, 3 - none (undefined)
#define PALETTE_SOLID_WRAP   (paletteBlend == 1 || paletteBlend == 3)

#define indexToVStrip(index, stripNr) ((index) | (int((stripNr)+1)<<16))

// static effect, used if an effect fails to initialize
static void mode_static(void) {
  SEGMENT.fill(SEGCOLOR(0));
}

#define FX_FALLBACK_STATIC { mode_static(); return; }

// If you define configuration options in your class and need to reference them in your effect function, add them here.
// If you only need to use them in your class you can define them as class members instead.
// bool myConfigValue = false;

/////////////////////////
//  User FX functions  //
/////////////////////////

// Diffusion Fire: fire effect intended for 2D setups smaller than 16x16
static void mode_diffusionfire(void) {
  if (!strip.isMatrix || !SEGMENT.is2D())
    FX_FALLBACK_STATIC;  // not a 2D set-up

  const int cols = SEG_W;
  const int rows = SEG_H;
  const auto XY = [&](int x, int y) { return x + y * cols; };

  const uint8_t refresh_hz = map(SEGMENT.speed, 0, 255, 20, 80);
  const unsigned refresh_ms = 1000 / refresh_hz;
  const int16_t diffusion = map(SEGMENT.custom1, 0, 255, 0, 100);
  const uint8_t spark_rate = SEGMENT.intensity;
  const uint8_t turbulence = SEGMENT.custom2;

unsigned dataSize = cols * rows;  // SEGLEN (virtual length) is equivalent to vWidth()*vHeight() for 2D
  if (!SEGENV.allocateData(dataSize))
    FX_FALLBACK_STATIC;  // allocation failed

  if (SEGENV.call == 0) {
    SEGMENT.fill(BLACK);
    SEGENV.step = 0;
  }

  if ((strip.now - SEGENV.step) >= refresh_ms) {
    // Keep for ≤~1 KiB; otherwise consider heap or reuse SEGENV.data as scratch.
    uint8_t tmp_row[cols];
    SEGENV.step = strip.now;
    // scroll up
    for (unsigned y = 1; y < rows; y++)
      for (unsigned x = 0; x < cols; x++) {
        unsigned src = XY(x, y);
        unsigned dst = XY(x, y - 1);
        SEGENV.data[dst] = SEGENV.data[src];
      }

    if (hw_random8() > turbulence) {
      // create new sparks at bottom row
      for (unsigned x = 0; x < cols; x++) {
        uint8_t p = hw_random8();
        if (p < spark_rate) {
          unsigned dst = XY(x, rows - 1);
          SEGENV.data[dst] = 255;
        }
      }
    }

    // diffuse
    for (unsigned y = 0; y < rows; y++) {
      for (unsigned x = 0; x < cols; x++) {
        unsigned v = SEGENV.data[XY(x, y)];
        if (x > 0) {
          v += SEGENV.data[XY(x - 1, y)];
        }
        if (x < (cols - 1)) {
          v += SEGENV.data[XY(x + 1, y)];
        }
        tmp_row[x] = min(255, (int)(v * 100 / (300 + diffusion)));
      }

      for (unsigned x = 0; x < cols; x++) {
        SEGENV.data[XY(x, y)] = tmp_row[x];
        if (SEGMENT.check1) {
          uint32_t color = SEGMENT.color_from_palette(tmp_row[x], true, false, 0);
          SEGMENT.setPixelColorXY(x, y, color);
        } else {
          uint32_t base = SEGCOLOR(0);
          SEGMENT.setPixelColorXY(x, y, color_fade(base, tmp_row[x]));
        }
      }
    }
  }
}
static const char _data_FX_MODE_DIFFUSIONFIRE[] PROGMEM = "Diffusion Fire@!,Spark rate,Diffusion Speed,Turbulence,,Use palette;;Color;;2;pal=35";


/*
 * Spinning Wheel effect - LED animates around 1D strip (or each column in a 2D matrix), slows down and stops at random position
 *  Created by Bob Loeffler and claude.ai
 *  First slider (Spin speed) is for the speed of the moving/spinning LED (random number within a narrow speed range).
 *     If value is 0, a random speed will be selected from the full range of values.
 *  Second slider (Spin slowdown start time) is for how long before the slowdown phase starts (random number within a narrow time range).
 *     If value is 0, a random time will be selected from the full range of values.
 *  Third slider (Spinner size) is for the number of pixels that make up the spinner.
 *  Fourth slider (Spin delay) is for how long it takes for the LED to start spinning again after the previous spin.
 *  The first checkbox allows the spinner to spin. If it's enabled, the spinner will do its thing. If it's not enabled, it will wait for the user to enable
 *     it either by clicking the checkbox or by pressing a physical button (e.g. using a playlist to run a couple presets that have JSON API codes).
 *  The second checkbox sets "color per block" mode. Enabled means that each spinner block will be the same color no matter what its LED position is.
 *  The third checkbox enables synchronized restart (all spinners restart together instead of individually).
 *  aux0 stores the settings checksum to detect changes
 *  aux1 stores the color scale for performance
 */

static void mode_spinning_wheel(void) {
  if (SEGLEN < 1) FX_FALLBACK_STATIC;

  unsigned strips = SEGMENT.nrOfVStrips();
  if (strips == 0) FX_FALLBACK_STATIC;

  constexpr unsigned stateVarsPerStrip = 8;
  unsigned dataSize = sizeof(uint32_t) * stateVarsPerStrip;
  if (!SEGENV.allocateData(dataSize * strips)) FX_FALLBACK_STATIC;
  uint32_t* state = reinterpret_cast<uint32_t*>(SEGENV.data);
  // state[0] = current position (fixed point: upper 16 bits = position, lower 16 bits = fraction)
  // state[1] = velocity (fixed point: pixels per frame * 65536)
  // state[2] = phase (0=fast spin, 1=slowing, 2=wobble, 3=stopped)
  // state[3] = stop time (when phase 3 was entered)
  // state[4] = wobble step (0=at stop pos, 1=moved back, 2=returned to stop)
  // state[5] = slowdown start time (when to transition from phase 0 to phase 1)
  // state[6] = wobble timing (for 200ms / 400ms / 300ms delays)
  // state[7] = store the stop position per strip

  // state[] index values for easier readability
  constexpr unsigned CUR_POS_IDX       = 0;  // state[0]
  constexpr unsigned VELOCITY_IDX      = 1;
  constexpr unsigned PHASE_IDX         = 2;
  constexpr unsigned STOP_TIME_IDX     = 3;
  constexpr unsigned WOBBLE_STEP_IDX   = 4;
  constexpr unsigned SLOWDOWN_TIME_IDX = 5;
  constexpr unsigned WOBBLE_TIME_IDX   = 6;
  constexpr unsigned STOP_POS_IDX      = 7;

  SEGMENT.fill(SEGCOLOR(1));

  // Handle random seeding globally (outside the virtual strip)
  if (SEGENV.call == 0) {
    SEGENV.aux1 = (255 << 8) / SEGLEN; // Cache the color scaling
  }

  // Check if settings changed (do this once, not per virtual strip)
  uint32_t settingssum = SEGMENT.speed + SEGMENT.intensity + SEGMENT.custom1 + SEGMENT.custom3 + SEGMENT.check1 + SEGMENT.check3;
  bool settingsChanged = (SEGENV.aux0 != settingssum);
  if (settingsChanged) {
    SEGENV.aux0 = settingssum;
  }

  // Check if all spinners are stopped and ready to restart (for synchronized restart)
  bool allReadyToRestart = true;
  if (SEGMENT.check3) {
    uint8_t spinnerSize = map(SEGMENT.custom1, 0, 255, 1, 10);
    uint16_t spin_delay = map(SEGMENT.custom3, 0, 31, 2000, 15000);
    uint32_t now = strip.now;

    for (unsigned stripNr = 0; stripNr < strips; stripNr += spinnerSize) {
      uint32_t* stripState = &state[stripNr * stateVarsPerStrip];
      // Check if this spinner is stopped AND has waited its delay
      if (stripState[PHASE_IDX] != 3 || stripState[STOP_TIME_IDX] == 0) {
        allReadyToRestart = false;
        break;
      }
      // Check if delay has elapsed
      if ((now - stripState[STOP_TIME_IDX]) < spin_delay) {
        allReadyToRestart = false;
        break;
      }
    }
  }
 
  struct virtualStrip {
    static void runStrip(uint16_t stripNr, uint32_t* state, bool settingsChanged, bool allReadyToRestart, unsigned strips) {
      uint8_t phase = state[PHASE_IDX];
      uint32_t now = strip.now;

      // Check for restart conditions
      bool needsReset = false;
      if (SEGENV.call == 0) {
        needsReset = true;
      } else if (settingsChanged && SEGMENT.check1) {
        needsReset = true;
      } else if (phase == 3 && state[STOP_TIME_IDX] != 0) {
          // If synchronized restart is enabled, only restart when all strips are ready
          if (SEGMENT.check3) {
            if (allReadyToRestart) {
              needsReset = true;
            }
          } else {
            // Normal mode: restart after individual strip delay
            uint16_t spin_delay = map(SEGMENT.custom3, 0, 31, 2000, 15000);
            if ((now - state[STOP_TIME_IDX]) >= spin_delay) {
              needsReset = true;
            }
          }
      }

      // Initialize or restart
      if (needsReset && SEGMENT.check1) {   // spin the spinner(s) only if the "Spin me!" checkbox is enabled
        state[CUR_POS_IDX] = 0;

        // Set velocity
        uint16_t speed = map(SEGMENT.speed, 0, 255, 300, 800);
        if (speed == 300) {  // random speed (user selected 0 on speed slider)
          state[VELOCITY_IDX] = hw_random16(200, 900) * 655;   // fixed-point velocity scaling (approx. 65536/100) 
        } else {
          state[VELOCITY_IDX] = hw_random16(speed - 100, speed + 100) * 655;
        }

        // Set slowdown start time
        uint16_t slowdown = map(SEGMENT.intensity, 0, 255, 3000, 5000);
        if (slowdown == 3000) {  // random slowdown start time (user selected 0 on intensity slider)
          state[SLOWDOWN_TIME_IDX] = now + hw_random16(2000, 6000);
        } else {
          state[SLOWDOWN_TIME_IDX] = now + hw_random16(slowdown - 1000, slowdown + 1000);
        }

        state[PHASE_IDX] = 0;
        state[STOP_TIME_IDX] = 0;
        state[WOBBLE_STEP_IDX] = 0;
        state[WOBBLE_TIME_IDX] = 0;
        state[STOP_POS_IDX] = 0; // Initialize stop position
        phase = 0;
      }

      uint32_t pos_fixed = state[CUR_POS_IDX];
      uint32_t velocity = state[VELOCITY_IDX];
      
      // Phase management
      if (phase == 0) {
        // Fast spinning phase
        if ((int32_t)(now - state[SLOWDOWN_TIME_IDX]) >= 0) {
          phase = 1;
          state[PHASE_IDX] = 1;
        }
      } else if (phase == 1) {
        // Slowing phase - apply deceleration
        uint32_t decel = velocity / 80;
        if (decel < 100) decel = 100;

        velocity = (velocity > decel) ? velocity - decel : 0;
        state[VELOCITY_IDX] = velocity;

        // Check if stopped
        if (velocity < 2000) {
          velocity = 0;
          state[VELOCITY_IDX] = 0;
          phase = 2;
          state[PHASE_IDX] = 2;
          state[WOBBLE_STEP_IDX] = 0;
          uint16_t stop_pos = (pos_fixed >> 16) % SEGLEN;
          state[STOP_POS_IDX] = stop_pos;
          state[WOBBLE_TIME_IDX] = now;
        }
      } else if (phase == 2) {
        // Wobble phase (moves the LED back one and then forward one)
        uint32_t wobble_step = state[WOBBLE_STEP_IDX];
        uint16_t stop_pos = state[STOP_POS_IDX];
        uint32_t elapsed = now - state[WOBBLE_TIME_IDX];

        if (wobble_step == 0 && elapsed >= 200) {
          // Move back one LED from stop position
          uint16_t back_pos = (stop_pos == 0) ? SEGLEN - 1 : stop_pos - 1;
          pos_fixed = ((uint32_t)back_pos) << 16;
          state[CUR_POS_IDX] = pos_fixed;
          state[WOBBLE_STEP_IDX] = 1;
          state[WOBBLE_TIME_IDX] = now;
        } else if (wobble_step == 1 && elapsed >= 400) {
          // Move forward to the stop position
          pos_fixed = ((uint32_t)stop_pos) << 16;
          state[CUR_POS_IDX] = pos_fixed;
          state[WOBBLE_STEP_IDX] = 2;
          state[WOBBLE_TIME_IDX] = now;
        } else if (wobble_step == 2 && elapsed >= 300) {
          // Wobble complete, enter stopped phase
          phase = 3;
          state[PHASE_IDX] = 3;
          state[STOP_TIME_IDX] = now;
        }
      }

      // Update position (phases 0 and 1 only)
      if (phase == 0 || phase == 1) {
        pos_fixed += velocity;
        state[CUR_POS_IDX] = pos_fixed;
      }

      // Draw LED for all phases
      uint16_t pos = (pos_fixed >> 16) % SEGLEN;

      uint8_t spinnerSize = map(SEGMENT.custom1, 0, 255, 1, 10);

      // Calculate color once per spinner block (based on strip number, not position)
      uint8_t hue;
      if (SEGMENT.check2) {
        // Each spinner block gets its own color based on strip number
        uint16_t numSpinners = max(1U, (strips + spinnerSize - 1) / spinnerSize);
        hue = (uint32_t)(255) * (stripNr / spinnerSize) / numSpinners;
      } else {
        // Color changes with position
        hue = (SEGENV.aux1 * pos) >> 8;
      }

      uint32_t color = ColorFromPalette(SEGPALETTE, hue, 255, LINEARBLEND);

      // Draw the spinner with configurable size (1-10 LEDs)
      for (int8_t x = 0; x < spinnerSize; x++) {
        for (uint8_t y = 0; y < spinnerSize; y++) {
          uint16_t drawPos = (pos + y) % SEGLEN;
          int16_t drawStrip = stripNr + x;

          // Wrap horizontally if needed, or skip if out of bounds
          if (drawStrip >= 0 && drawStrip < strips) {
            SEGMENT.setPixelColor(indexToVStrip(drawPos, drawStrip), color);
          }
        }
      }
    }
  };

  for (unsigned stripNr = 0; stripNr < strips; stripNr++) {
    // Only run on strips that are multiples of spinnerSize to avoid overlap
    uint8_t spinnerSize = map(SEGMENT.custom1, 0, 255, 1, 10);
    if (stripNr % spinnerSize == 0) {
      virtualStrip::runStrip(stripNr, &state[stripNr * stateVarsPerStrip], settingsChanged, allReadyToRestart, strips);
    }
  }
}
static const char _data_FX_MODE_SPINNINGWHEEL[] PROGMEM = "Spinning Wheel@Speed (0=random),Slowdown (0=random),Spinner size,,Spin delay,Spin me!,Color per block,Sync restart;!,!;!;;m12=1,c1=1,c3=8,o1=1,o3=1";


/*
/  Lava Lamp 2D effect
*   Uses particles to simulate rising blobs of "lava" or wax
*   Particles slowly rise, merge to create organic flowing shapes, and then fall to the bottom to start again
*   Created by Bob Loeffler using claude.ai
*   The first slider sets the number of active blobs
*   The second slider sets the size range of the blobs
*   The third slider sets the damping value for horizontal blob movement
*   The Attract checkbox sets the attraction of blobs (checked will make the blobs attract other close blobs horizontally)
*   The Keep Color Ratio checkbox sets whether we preserve the color ratio when displaying pixels that are in 2 or more overlapping blobs
*   aux0 keeps track of the blob size value
*   aux1 keeps track of the number of blobs
*/

typedef struct LavaParticle {
  float    x, y;         // Position
  float    vx, vy;       // Velocity
  float    size;         // Blob size
  uint8_t  hue;          // Color
  bool     active;       // will not be displayed if false
  uint16_t delayTop;     // number of frames to wait at top before falling again
  bool     idleTop;      // sitting idle at the top
  uint16_t delayBottom;  // number of frames to wait at bottom before rising again
  bool     idleBottom;   // sitting idle at the bottom
} LavaParticle;

static void mode_2D_lavalamp(void) {
  if (!strip.isMatrix || !SEGMENT.is2D()) FX_FALLBACK_STATIC; // not a 2D set-up

  const uint16_t cols = SEG_W;
  const uint16_t rows = SEG_H;
  constexpr float MAX_BLOB_RADIUS = 20.0f;  // cap to prevent frame rate drops on large matrices
  constexpr size_t MAX_LAVA_PARTICLES = 34;  // increasing this value could cause slowness for large matrices
  constexpr size_t MAX_TOP_FPS_DELAY = 900;  // max delay when particles are at the top
  constexpr size_t MAX_BOTTOM_FPS_DELAY = 1200;  // max delay when particles are at the bottom

  // Allocate per-segment storage
  if (!SEGENV.allocateData(sizeof(LavaParticle) * MAX_LAVA_PARTICLES)) FX_FALLBACK_STATIC;
  LavaParticle* lavaParticles = reinterpret_cast<LavaParticle*>(SEGENV.data);

  // Initialize particles on first call
  if (SEGENV.call == 0) {
    for (int i = 0; i < MAX_LAVA_PARTICLES; i++) {
      lavaParticles[i].active = false;
    }
  }

  // Track particle size and particle count slider changes, re-initialize if either changes
  uint8_t currentNumParticles = (SEGMENT.intensity >> 3) + 3;
  uint8_t currentSize = SEGMENT.custom1;
  if (currentNumParticles > MAX_LAVA_PARTICLES) currentNumParticles = MAX_LAVA_PARTICLES;
  bool needsReinit = (currentSize != SEGENV.aux0) || (currentNumParticles != SEGENV.aux1);

  if (needsReinit) {
    for (int i = 0; i < MAX_LAVA_PARTICLES; i++) {
      lavaParticles[i].active = false;
    }
    SEGENV.aux0 = currentSize;
    SEGENV.aux1 = currentNumParticles;
  }

  uint8_t size = currentSize;
  uint8_t numParticles = currentNumParticles;

  // blob size based on matrix width
  const float minSize = cols * 0.15f; // Minimum 15% of width
  const float maxSize = cols * 0.4f;  // Maximum 40% of width
  float sizeRange = (maxSize - minSize) * (size / 255.0f);
  int rangeInt = max(1, (int)(sizeRange));

  // calculate the spawning area for the particles
  const float spawnXStart = cols * 0.20f;
  const float spawnXWidth = cols * 0.60f;
  int spawnX = max(1, (int)(spawnXWidth));

  bool preserveColorRatio = SEGMENT.check3;

  // Spawn new particles at the bottom near the center
  for (int i = 0; i < MAX_LAVA_PARTICLES; i++) {
    if (!lavaParticles[i].active && hw_random8() < 32) { // spawn when slot available
      // Spawn in the middle 60% of the matrix width
      lavaParticles[i].x = spawnXStart + (float)hw_random16(spawnX);
      lavaParticles[i].y = rows - 1;
      lavaParticles[i].vx = (hw_random16(7) - 3) / 250.0f;
      lavaParticles[i].vy = -(hw_random16(20) + 10) / 100.0f * 0.3f;

      lavaParticles[i].size = minSize + (float)hw_random16(rangeInt);
      if (lavaParticles[i].size > MAX_BLOB_RADIUS) lavaParticles[i].size = MAX_BLOB_RADIUS;

      lavaParticles[i].hue = hw_random8();
      lavaParticles[i].active = true;

      // Set random delays when particles are at top and bottom
      lavaParticles[i].delayTop = hw_random16(MAX_TOP_FPS_DELAY);
      lavaParticles[i].delayBottom = hw_random16(MAX_BOTTOM_FPS_DELAY);
      lavaParticles[i].idleBottom = true;
      break;
    }
  }

  // Fade background slightly for trailing effect
  SEGMENT.fadeToBlackBy(40);

  // Update and draw particles
  int activeCount = 0;
  unsigned long currentMillis = strip.now;
  for (int i = 0; i < MAX_LAVA_PARTICLES; i++) {
    if (!lavaParticles[i].active) continue;
    activeCount++;

    // Keep particle count on target by deactivating excess particles
    if (activeCount > numParticles) {
      lavaParticles[i].active = false;
      activeCount--;
      continue;
    }

    LavaParticle *p = &lavaParticles[i];

    // Physics update
    p->x += p->vx;
    p->y += p->vy;

    // Optional particle/blob attraction
    if (SEGMENT.check2) {
      for (int j = 0; j < MAX_LAVA_PARTICLES; j++) {
        if (i == j || !lavaParticles[j].active) continue;

        LavaParticle *other = &lavaParticles[j];

        // Skip attraction if moving in same vertical direction (both up or both down)
        if ((p->vy < 0 && other->vy < 0) || (p->vy > 0 && other->vy > 0)) continue;

        float dx = other->x - p->x;
        float dy = other->y - p->y;

        // Apply weak horizontal attraction only
        float attractRange = p->size + other->size;
        float distSq = dx*dx + dy*dy;
        float attractRangeSq = attractRange * attractRange;
        if (distSq > 0 && distSq < attractRangeSq) {
          float dist = sqrt(distSq); // Only compute sqrt when needed
          float force = (1.0f - (dist / attractRange)) * 0.0001f;
          p->vx += (dx / dist) * force;
        }
      }
    }

    // Horizontal oscillation (makes it more organic)
    float damping= map(SEGMENT.custom2, 0, 255, 97, 87) / 100.0f;
    p->vx += sin((currentMillis / 1000.0f + i) * 0.5f) * 0.002f; // Reduced oscillation
    p->vx *= damping; // damping for more or less horizontal drift

    // Bounce off sides (don't affect vertical velocity)
    if (p->x < 0) {
      p->x = 0;
      p->vx = abs(p->vx); // reverse horizontal
    }
    if (p->x >= cols) {
      p->x = cols - 1;
      p->vx = -abs(p->vx); // reverse horizontal
    }

    // Adjust rise/fall velocity depending on approx distance from heat source (at bottom)
    // In top 1/4th of rows...
    if (p->y < rows * .25f) {
      if (p->vy >= 0) {  // if going down, delay the particles so they won't go down immediately
        if (p->delayTop > 0 && p->idleTop) {
          p->vy = 0.0f;
          p->delayTop--;
          p->idleTop = true;
        } else {
          p->vy = 0.01f;
          p->delayTop = hw_random16(MAX_TOP_FPS_DELAY);
          p->idleTop = false;
        }
      } else if (p->vy <= 0) {  // if going up, slow down the rise rate
        p->vy = -0.03f;
      }
    }

    // In next 1/4th of rows...
    if (p->y <= rows * .50f && p->y >= rows * .25f) {
      if (p->vy > 0) {  // if going down, speed up the fall rate
        p->vy = 0.03f;
      } else if (p->vy <= 0) {  // if going up, speed up the rise rate a little more
        p->vy = -0.05f;
      }
    }

    // In next 1/4th of rows...
    if (p->y <= rows * .75f && p->y >= rows * .50f) {
      if (p->vy > 0) {  // if going down, speed up the fall rate a little more
        p->vy = 0.04f;
      } else if (p->vy <= 0) {  // if going up, speed up the rise rate
        p->vy = -0.03f;
      }
    }

    // In bottom 1/4th of rows...
    if (p->y > rows * .75f) {
      if (p->vy >= 0) {  // if going down, slow down the fall rate
        p->vy = 0.02f;
      } else if (p->vy <= 0) {  // if going up, delay the particles so they won't go up immediately
        if (p->delayBottom > 0 && p->idleBottom) {
          p->vy = 0.0f;
          p->delayBottom--;
          p->idleBottom = true;
        } else {
          p->vy = -0.01f;
          p->delayBottom = hw_random16(MAX_BOTTOM_FPS_DELAY);
          p->idleBottom = false;
        }
      }
    }

    // Boundary handling with reversal of direction
    // When reaching TOP (y=0 area), reverse to fall back down, but need to delay first
    if (p->y <= 0.5f * p->size) {
      p->y = 0.5f * p->size;
      if (p->vy < 0) {
        p->vy = 0.005f;  // set to a tiny positive value to start falling very slowly
        p->idleTop = true;
      }
    }

    // When reaching BOTTOM (y=rows-1 area), reverse to rise back up, but need to delay first
    if (p->y >= rows - 0.5f * p->size) {
      p->y = rows - 0.5f * p->size;
      if (p->vy > 0) {
        p->vy = -0.005f;  // set to a tiny negative value to start rising very slowly
        p->idleBottom = true;
      }
    }

    // Get color
    uint32_t color;
    color = SEGMENT.color_from_palette(p->hue, true, PALETTE_SOLID_WRAP, 0);

    // Extract RGB and apply life/opacity
    uint8_t w = (W(color) * 255) >> 8;
    uint8_t r = (R(color) * 255) >> 8;
    uint8_t g = (G(color) * 255) >> 8;
    uint8_t b = (B(color) * 255) >> 8;

    // Draw blob with sub-pixel accuracy using bilinear distribution
    float sizeSq = p->size * p->size;

    // Get fractional offsets of particle center
    float fracX = p->x - floorf(p->x);
    float fracY = p->y - floorf(p->y);
    int centerX = (int)floorf(p->x);
    int centerY = (int)floorf(p->y);

    for (int dy = -(int)p->size - 1; dy <= (int)p->size + 1; dy++) {
      for (int dx = -(int)p->size - 1; dx <= (int)p->size + 1; dx++) {
        int px = centerX + dx;
        int py = centerY + dy;

        if (px < 0 || px >= cols || py < 0 || py >= rows) continue;

        // Sub-pixel distance: measure from true float center to pixel center
        float subDx = dx - fracX;  // distance from true center to this pixel's center
        float subDy = dy - fracY;
        float distSq = subDx * subDx + subDy * subDy;

        if (distSq < sizeSq) {
          float intensity = 1.0f - (distSq / sizeSq);
          intensity = intensity * intensity; // smooth falloff

          uint8_t bw = (uint8_t)(w * intensity);
          uint8_t br = (uint8_t)(r * intensity);
          uint8_t bg = (uint8_t)(g * intensity);
          uint8_t bb = (uint8_t)(b * intensity);

          uint32_t existing = SEGMENT.getPixelColorXY(px, py);
          uint32_t newColor = RGBW32(br, bg, bb, bw);
          SEGMENT.setPixelColorXY(px, py, color_add(existing, newColor, preserveColorRatio ? true : false));
        }
      }
    }
  }
}
static const char _data_FX_MODE_2D_LAVALAMP[] PROGMEM = "Lava Lamp@,# of blobs,Blob size,H. Damping,,,Attract,Keep Color Ratio;;!;2;ix=64,c2=192,o2=1,o3=1,pal=47";


/*
/  Magma effect
*   2D magma/lava animation
*   Adapted from FireLamp_JeeUI implementation (https://github.com/DmytroKorniienko/FireLamp_JeeUI/tree/dev)
*   Original idea by SottNick, remastered by kostyamat
*   Adapted to WLED by Bob Loeffler and claude.ai
*   First slider (speed) is for the speed or flow rate of the moving magma.
*   Second slider (intensity) is for the height of the magma.
*   Third slider (lava bombs) is for the number of lava bombs (particles).  The max # is 1/2 the number of columns on the 2D matrix.
*   Fourth slider (gravity) is for how high the lava bombs will go.
*   The checkbox (check2) is for whether the lava bombs can be seen in the magma or behind it.
*/

// Draw the magma
static void drawMagma(const uint16_t width, const uint16_t height, float *ff_y, float *ff_z, uint8_t *shiftHue) {
  // Noise parameters - adjust these for different magma characteristics
  // deltaValue: higher = more detailed/turbulent magma
  // deltaHue: higher = taller magma structures
  constexpr uint8_t magmaDeltaValue = 12U;
  constexpr uint8_t magmaDeltaHue   = 10U;

  uint16_t ff_y_int = (uint16_t)*ff_y;
  uint16_t ff_z_int = (uint16_t)*ff_z;

  for (uint16_t i = 0; i < width; i++) {
    for (uint16_t j = 0; j < height; j++) {
      // Generate Perlin noise value (0-255)
      uint8_t noise = perlin8(i * magmaDeltaValue, (j + ff_y_int + hw_random8(2)) * magmaDeltaHue, ff_z_int);
      uint8_t paletteIndex = qsub8(noise, shiftHue[j]);  // Apply the vertical fade gradient
      CRGB col = SEGMENT.color_from_palette(paletteIndex, false, PALETTE_SOLID_WRAP, 0);  // Get color from palette
      SEGMENT.addPixelColorXY(i, height - 1 - j, col);  // magma rises from bottom of display
    }
  }
}

// Move and draw lava bombs (particles)
static void drawLavaBombs(const uint16_t width, const uint16_t height, float *particleData, float gravity, uint8_t particleCount) {
  for (uint16_t i = 0; i < particleCount; i++) {
    uint16_t idx = i * 4;

    particleData[idx + 3] -= gravity;
    particleData[idx + 0] += particleData[idx + 2];
    particleData[idx + 1] += particleData[idx + 3];

    float posX = particleData[idx + 0];
    float posY = particleData[idx + 1];

    if (posY > height + height / 4) {
      particleData[idx + 3] = -particleData[idx + 3] * 0.8f;
    }

    if (posY < (float)(height / 8) - 1.0f || posX < 0 || posX >= width) {
      particleData[idx + 0] = hw_random(0, width * 100) / 100.0f;
      particleData[idx + 1] = hw_random(0, height * 25) / 100.0f;
      particleData[idx + 2] = hw_random(-75, 75) / 100.0f;

      float baseVelocity = hw_random(60, 120) / 100.0f;
      if (hw_random8() < 50) {
        baseVelocity *= 1.6f;
      }
      particleData[idx + 3] = baseVelocity;
      continue;
    }

    int16_t xi = (int16_t)posX;
    int16_t yi = (int16_t)posY;

    if (xi >= 0 && xi < width && yi >= 0 && yi < height) {
      // Get a random color from the current palette
      uint8_t randomIndex = hw_random8(64, 128);
      CRGB pcolor = ColorFromPalette(SEGPALETTE, randomIndex, 255, LINEARBLEND);

      // Pre-calculate anti-aliasing weights
      float xf = posX - xi;
      float yf = posY - yi;
      float ix = 1.0f - xf;
      float iy = 1.0f - yf;

      uint8_t w0 = 255 * ix * iy;
      uint8_t w1 = 255 * xf * iy;
      uint8_t w2 = 255 * ix * yf;
      uint8_t w3 = 255 * xf * yf;

      int16_t yFlipped = height - 1 - yi;  // Flip Y coordinate

      SEGMENT.addPixelColorXY(xi, yFlipped, pcolor.scale8(w0));
      if (xi + 1 < width) 
        SEGMENT.addPixelColorXY(xi + 1, yFlipped, pcolor.scale8(w1));
      if (yFlipped - 1 >= 0)
        SEGMENT.addPixelColorXY(xi, yFlipped - 1, pcolor.scale8(w2));
      if (xi + 1 < width && yFlipped - 1 >= 0)
        SEGMENT.addPixelColorXY(xi + 1, yFlipped - 1, pcolor.scale8(w3));
    }
  }
}

static void mode_2D_magma(void) {
  if (!strip.isMatrix || !SEGMENT.is2D()) FX_FALLBACK_STATIC;  // not a 2D set-up
  const uint16_t width = SEG_W;
  const uint16_t height = SEG_H;
  const uint8_t MAGMA_MAX_PARTICLES = width / 2;
  if (MAGMA_MAX_PARTICLES < 2) FX_FALLBACK_STATIC;  // matrix too narrow for lava bombs
  constexpr size_t SETTINGS_SUM_BYTES = 4; // 4 bytes for settings sum

  // Allocate memory: particles (4 floats each) + 2 floats for noise counters + shiftHue cache + settingsSum
  const uint16_t dataSize = (MAGMA_MAX_PARTICLES * 4 + 2) * sizeof(float) + height * sizeof(uint8_t) + SETTINGS_SUM_BYTES;
  if (!SEGENV.allocateData(dataSize)) FX_FALLBACK_STATIC;  // allocation failed

  float* particleData = reinterpret_cast<float*>(SEGENV.data);
  float* ff_y = &particleData[MAGMA_MAX_PARTICLES * 4];
  float* ff_z = &particleData[MAGMA_MAX_PARTICLES * 4 + 1];
  uint32_t* settingsSumPtr = reinterpret_cast<uint32_t*>(&particleData[MAGMA_MAX_PARTICLES * 4 + 2]);
  uint8_t* shiftHue = reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(settingsSumPtr) + SETTINGS_SUM_BYTES);

  // Check if settings changed
  uint32_t settingsKey = (uint32_t)SEGMENT.speed | ((uint32_t)SEGMENT.intensity << 8) |
      ((uint32_t)SEGMENT.custom1 << 16) | ((uint32_t)SEGMENT.custom2 << 24);
  bool settingsChanged = (*settingsSumPtr != settingsKey);

  if (SEGENV.call == 0 || settingsChanged) {
    // Intensity slider controls magma height
    uint16_t intensity = SEGMENT.intensity;
    uint16_t fadeRange = map(intensity, 0, 255, height / 3, height);

    // shiftHue controls the vertical color gradient (magma fades out toward top)
    for (uint16_t j = 0; j < height; j++) {
      if (j < fadeRange) {
        // prevent division issues and ensure smooth gradient
        if (fadeRange > 1) {
          shiftHue[j] = (uint8_t)(j * 255 / (fadeRange - 1));
        } else {
          shiftHue[j] = 0;  // Single row magma = no fade
        }
      } else {
        shiftHue[j] = 255;
      }
    }

    // Initialize all particles
    for (uint16_t i = 0; i < MAGMA_MAX_PARTICLES; i++) {
      uint16_t idx = i * 4;
      particleData[idx + 0] = hw_random(0, width * 100) / 100.0f;
      particleData[idx + 1] = hw_random(0, height * 25) / 100.0f;
      particleData[idx + 2] = hw_random(-75, 75) / 100.0f;

      float baseVelocity = hw_random(60, 120) / 100.0f;
      if (hw_random8() < 50) {
        baseVelocity *= 1.6f;
      }
      particleData[idx + 3] = baseVelocity;
    }
    *ff_y = 0.0f;
    *ff_z = 0.0f;
    *settingsSumPtr = settingsKey;
  }

  if (!shiftHue) FX_FALLBACK_STATIC;   // safety check

  // Speed control
  float speedfactor = SEGMENT.speed / 255.0f;
  speedfactor = speedfactor * speedfactor * 1.5f;
  if (speedfactor < 0.001f) speedfactor = 0.001f;

  // Gravity control
  float gravity = map(SEGMENT.custom2, 0, 255, 5, 20) / 100.0f;

  // Number of particles (lava bombs)
  uint8_t particleCount = map(SEGMENT.custom1, 0, 255, 0, MAGMA_MAX_PARTICLES);
  particleCount = constrain(particleCount, 0, MAGMA_MAX_PARTICLES);

  // Draw lava bombs in front of magma (or behind it)
  if (SEGMENT.check2) {
    drawMagma(width, height, ff_y, ff_z, shiftHue);
    SEGMENT.fadeToBlackBy(70);    // Dim the entire display to create trailing effect
    if (particleCount > 0) drawLavaBombs(width, height, particleData, gravity, particleCount);
  }
  else {
    if (particleCount > 0) drawLavaBombs(width, height, particleData, gravity, particleCount);
    SEGMENT.fadeToBlackBy(70);    // Dim the entire display to create trailing effect
    drawMagma(width, height, ff_y, ff_z, shiftHue);
  }

  // noise counters based on speed slider
  *ff_y += speedfactor * 2.0f;
  *ff_z += speedfactor;

  SEGENV.step++;
}
static const char _data_FX_MODE_2D_MAGMA[] PROGMEM = "Magma@Flow rate,Magma height,Lava bombs,Gravity,,,Bombs in front;;!;2;ix=192,c2=32,o2=1,pal=35";


/*
/  Ants (created by making modifications to the Rolling Balls code) - Bob Loeffler 2025
*   First slider is for the ants' speed.
*   Second slider is for the # of ants.
*   Third slider is for the Ants' size.
*   Fourth slider (custom2) is for blurring the LEDs in the segment.
*   Checkbox1 is for Gathering food (enabled if you want the ants to gather food, disabled if they are just walking).
*     We will switch directions when they get to the beginning or end of the segment when gathering food.
*     When gathering food, the Pass By option will automatically be enabled so they can drop off their food easier (and look for more food).
*   Checkbox2 is for Smear mode (enabled is smear pixel colors, disabled is no smearing)
*   Checkbox3 is for whether the ants will bump into each other (disabled) or just pass by each other (enabled)
*/

// Ant structure representing each ant's state
struct Ant {
  unsigned long lastBumpUpdate;  // the last time the ant bumped into another ant
  bool hasFood;
  float velocity;
  float position;  // (0.0 to 1.0 range)
};

constexpr unsigned MAX_ANTS = 32;
constexpr float MIN_COLLISION_TIME_MS = 2.0f;
constexpr float VELOCITY_MIN = 2.0f;
constexpr float VELOCITY_MAX = 10.0f;
constexpr unsigned ANT_SIZE_MIN = 1;
constexpr unsigned ANT_SIZE_MAX = 20;

// Helper function to get food pixel color based on ant and background colors
static uint32_t getFoodColor(uint32_t antColor, uint32_t backgroundColor) {
  if (antColor == WHITE)
    return (backgroundColor == YELLOW) ? GRAY : YELLOW;
  return (backgroundColor == WHITE) ? YELLOW : WHITE;
}

// Helper function to handle ant boundary wrapping or bouncing
static void handleBoundary(Ant& ant, float& position, bool gatherFood, bool atStart, unsigned long currentTime) {
  if (gatherFood) {
    // Bounce mode: reverse direction and update food status
    position = atStart ? 0.0f : 1.0f;
    ant.velocity = -ant.velocity;
    ant.lastBumpUpdate = currentTime;
    ant.position = position;
    ant.hasFood = atStart;  // Has food when leaving start, drops it at end
  } else {
    // Wrap mode: teleport to opposite end
    position = atStart ? 1.0f : 0.0f;
    ant.lastBumpUpdate = currentTime;
    ant.position = position;
  }
}

// Helper function to calculate ant color
static uint32_t getAntColor(int antIndex, int numAnts, bool usePalette) {
  if (usePalette)
    return SEGMENT.color_from_palette(antIndex * 255 / numAnts, false, (paletteBlend == 1 || paletteBlend == 3), 255);
  // Alternate between two colors for default palette
  return (antIndex % 3 == 1) ? SEGCOLOR(0) : SEGCOLOR(2);
}

// Helper function to render a single ant pixel with food handling
static void renderAntPixel(int pixelIndex, int pixelOffset, int antSize, const Ant& ant, uint32_t antColor, uint32_t backgroundColor, bool gatherFood) {
  bool isMovingBackward = (ant.velocity < 0);
  bool isFoodPixel = gatherFood && ant.hasFood && ((isMovingBackward && pixelOffset == 0) || (!isMovingBackward && pixelOffset == antSize - 1));
  if (isFoodPixel) {
    SEGMENT.setPixelColor(pixelIndex, getFoodColor(antColor, backgroundColor));
  } else {
    SEGMENT.setPixelColor(pixelIndex, antColor);
  }
}

static void mode_ants(void) {
  if (SEGLEN <= 1) FX_FALLBACK_STATIC;

  // Allocate memory for ant data
  uint32_t backgroundColor = SEGCOLOR(1);
  unsigned dataSize = sizeof(Ant) * MAX_ANTS;
  if (!SEGENV.allocateData(dataSize)) FX_FALLBACK_STATIC;  // Allocation failed

  Ant* ants = reinterpret_cast<Ant*>(SEGENV.data);

  // Extract configuration from segment settings
  unsigned numAnts = min(1 + (SEGLEN * SEGMENT.intensity >> 12), MAX_ANTS);
  bool gatherFood = SEGMENT.check1;
  bool SmearMode = SEGMENT.check2;
  bool passBy = SEGMENT.check3 || gatherFood;  // global no‑collision when gathering food is enabled
  unsigned antSize = map(SEGMENT.custom1, 0, 255, ANT_SIZE_MIN, ANT_SIZE_MAX) + (gatherFood ? 1 : 0);

  // Initialize ants on first call
  if (SEGENV.call == 0) {
    int confusedAntIndex = hw_random(0, numAnts);   // the first random ant to go backwards

    for (int i = 0; i < MAX_ANTS; i++) {
      ants[i].lastBumpUpdate = strip.now;

      // Random velocity
      float velocity = VELOCITY_MIN + (VELOCITY_MAX - VELOCITY_MIN) * hw_random16(1000, 5000) / 5000.0f;
      // One random ant moves in opposite direction
      ants[i].velocity = (i == confusedAntIndex) ? -velocity : velocity;
      // Random starting position (0.0 to 1.0)
      ants[i].position = hw_random16(0, 10000) / 10000.0f;
      // Ants don't have food yet
      ants[i].hasFood = false;
    }
  }

  // Calculate time conversion factor based on speed slider
  float timeConversionFactor = float(scale8(8, 255 - SEGMENT.speed) + 1) * 20000.0f;

  // Clear background if not in Smear mode
  if (!SmearMode) SEGMENT.fill(backgroundColor);

  // Update and render each ant
  for (int i = 0; i < numAnts; i++) {
    float timeSinceLastUpdate = float(int(strip.now - ants[i].lastBumpUpdate)) / timeConversionFactor;
    float newPosition = ants[i].position + ants[i].velocity * timeSinceLastUpdate;

    // Reset ants that wandered too far off-track (e.g., after intensity change)
    if (newPosition < -0.5f || newPosition > 1.5f) {
      newPosition = ants[i].position = hw_random16(0, 10000) / 10000.0f;
      ants[i].lastBumpUpdate = strip.now;
    }

    // Handle boundary conditions (bounce or wrap)
    if (newPosition <= 0.0f && ants[i].velocity < 0.0f) {
      handleBoundary(ants[i], newPosition, gatherFood, true, strip.now);
    } else if (newPosition >= 1.0f && ants[i].velocity > 0.0f) {
      handleBoundary(ants[i], newPosition, gatherFood, false, strip.now);
    }

    // Handle collisions between ants (if not passing by)
    if (!passBy) {
      for (int j = i + 1; j < numAnts; j++) {
        if (fabsf(ants[j].velocity - ants[i].velocity) < 0.001f) continue;  // Moving in same direction at same speed; avoids tiny denominators

        // Calculate collision time using physics -  collisionTime formula adapted from rolling_balls
        float timeOffset = float(int(ants[j].lastBumpUpdate - ants[i].lastBumpUpdate));
        float collisionTime = (timeConversionFactor * (ants[i].position - ants[j].position) + ants[i].velocity * timeOffset) / (ants[j].velocity - ants[i].velocity);

        // Check if collision occurred in valid time window
        float timeSinceJ = float(int(strip.now - ants[j].lastBumpUpdate));
        if (collisionTime > MIN_COLLISION_TIME_MS && collisionTime < timeSinceJ) {
          // Update positions to collision point
          float adjustedTime = (collisionTime + float(int(ants[j].lastBumpUpdate - ants[i].lastBumpUpdate))) / timeConversionFactor;
          ants[i].position += ants[i].velocity * adjustedTime;
          ants[j].position = ants[i].position;

          // Update collision time
          unsigned long collisionMoment = static_cast<unsigned long>(collisionTime + 0.5f) + ants[j].lastBumpUpdate;
          ants[i].lastBumpUpdate = collisionMoment;
          ants[j].lastBumpUpdate = collisionMoment;

          // Reverse the ant with greater speed magnitude
          if (fabsf(ants[i].velocity) > fabsf(ants[j].velocity)) {
            ants[i].velocity = -ants[i].velocity;
          } else {
            ants[j].velocity = -ants[j].velocity;
          }

          // Recalculate position after collision
          newPosition = ants[i].position + ants[i].velocity * float(int(strip.now - ants[i].lastBumpUpdate)) / timeConversionFactor;
        }
      }
    }

    // Clamp position to valid range
    newPosition = constrain(newPosition, 0.0f, 1.0f);
    unsigned pixelPosition = roundf(newPosition * (SEGLEN - 1));

    // Determine ant color
    uint32_t antColor = getAntColor(i, numAnts, SEGMENT.palette != 0);

    // Render ant pixels
    for (int pixelOffset = 0; pixelOffset < antSize; pixelOffset++) {
      unsigned currentPixel = pixelPosition + pixelOffset;
      if (currentPixel >= SEGLEN) break;
      renderAntPixel(currentPixel, pixelOffset, antSize, ants[i], antColor, backgroundColor, gatherFood);
    }

    // Update ant state
    ants[i].lastBumpUpdate = strip.now;
    ants[i].position = newPosition;
  }

  SEGMENT.blur(SEGMENT.custom2>>1);
}
static const char _data_FX_MODE_ANTS[] PROGMEM = "Ants@Ant speed,# of ants,Ant size,Blur,,Gathering food,Smear,Pass by;!,!,!;!;1;sx=192,ix=255,c1=32,c2=0,o1=1,o3=1";


/*
/  Morse Code by Bob Loeffler
*   Adapted from code by automaticaddison.com and then optimized by claude.ai
*   aux0 is the pattern offset for scrolling
*   aux1 saves settings: check2 (1 bit), check3 (1 bit), text hash (4 bits) and pattern length (10 bits)
*   The first slider (sx) selects the scrolling speed
*   The second slider selects the color mode (lower half selects color wheel, upper half selects color palettes)
*   Checkbox1 displays all letters in a word with the same color
*   Checkbox2 displays punctuation or not
*   Checkbox3 displays the End-of-message code or not
*   We get the text from the SEGMENT.name and convert it to morse code
*   This effect uses a bit array, instead of bool array, for efficient storage - 8x memory reduction (128 bytes vs 1024 bytes)
*
*   Morse Code rules:
*    - a dot is 1 pixel/LED; a dash is 3 pixels/LEDs
*    - there is 1 space between each dot or dash that make up a letter/number/punctuation
*    - there are 3 spaces between each letter/number/punctuation
*    - there are 7 spaces between each word
*/

// Bit manipulation macros
#define SET_BIT8(arr, i) ((arr)[(i) >> 3] |= (1 << ((i) & 7)))
#define GET_BIT8(arr, i) (((arr)[(i) >> 3] & (1 << ((i) & 7))) != 0)

// Build morse code pattern into a buffer
static void build_morsecode_pattern(const char *morse_code, uint8_t *pattern, uint8_t *wordIndex, uint16_t &index, uint8_t currentWord, int maxSize) {
  const char *c = morse_code;

  // Build the dots and dashes into pattern array
  while (*c != '\0') {
    // it's a dot which is 1 pixel
    if (*c == '.') {
      if (index >= maxSize - 1) return;
      SET_BIT8(pattern, index);
      wordIndex[index] = currentWord;
      index++;
    }
    else { // Must be a dash which is 3 pixels
      if (index >= maxSize - 3) return;
      SET_BIT8(pattern, index);
      wordIndex[index] = currentWord;
      index++;
      SET_BIT8(pattern, index);
      wordIndex[index] = currentWord;
      index++;
      SET_BIT8(pattern, index);
      wordIndex[index] = currentWord;
      index++;
    }

    c++;

    // 1 space between parts of a letter/number/punctuation (but not after the last one)
    if (*c != '\0') {
      if (index >= maxSize) return;
      wordIndex[index] = currentWord;
      index++;
    }
  }

  // 3 spaces between two letters/numbers/punctuation
  if (index >= maxSize - 2) return;
  wordIndex[index] = currentWord;
  index++;
  if (index >= maxSize - 1) return;
  wordIndex[index] = currentWord;
  index++;
  if (index >= maxSize) return;
  wordIndex[index] = currentWord;
  index++;
}

static void mode_morsecode(void) {
  if (SEGLEN < 1) FX_FALLBACK_STATIC;

  // A-Z in Morse Code
  static const char * letters[] = {".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--",
                     "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--.."};
  // 0-9 in Morse Code
  static const char * numbers[] = {"-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----."};

  // Punctuation in Morse Code
  struct PunctuationMapping {
    char character;
    const char* code;
  };

  static const PunctuationMapping punctuation[] = {
    {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, 
    {':', "---..."}, {'-', "-....-"}, {'!', "-.-.--"},
    {'&', ".-..."}, {'@', ".--.-."}, {')', "-.--.-"},
    {'(', "-.--."}, {'/', "-..-."}, {'\'', ".----."}
  };

  // Get the text to display
  char text[WLED_MAX_SEGNAME_LEN+1] = {'\0'};
  size_t len = 0;

  if (SEGMENT.name) len = strlen(SEGMENT.name);
  if (len == 0) {
    strcpy_P(text, PSTR("I Love WLED!"));
  } else {
    strcpy(text, SEGMENT.name);
  }

  // Convert to uppercase in place
  for (char *p = text; *p; p++) {
    *p = toupper(*p);
  }

  // Allocate per-segment storage for pattern (1023 bits = 127 bytes) + word index array (1024 bytes) + word count (1 byte)
  constexpr size_t MORSECODE_MAX_PATTERN_SIZE = 1023;
  constexpr size_t MORSECODE_PATTERN_BYTES = (MORSECODE_MAX_PATTERN_SIZE + 7) / 8; // 128 bytes
  constexpr size_t MORSECODE_WORD_INDEX_BYTES = MORSECODE_MAX_PATTERN_SIZE; // 1 byte per bit position
  constexpr size_t MORSECODE_WORD_COUNT_BYTES = 1; // 1 byte for word count
  if (!SEGENV.allocateData(MORSECODE_PATTERN_BYTES + MORSECODE_WORD_INDEX_BYTES + MORSECODE_WORD_COUNT_BYTES)) FX_FALLBACK_STATIC;
  uint8_t* morsecodePattern = reinterpret_cast<uint8_t*>(SEGENV.data);
  uint8_t* wordIndexArray = reinterpret_cast<uint8_t*>(SEGENV.data + MORSECODE_PATTERN_BYTES);
  uint8_t* wordCountPtr = reinterpret_cast<uint8_t*>(SEGENV.data + MORSECODE_PATTERN_BYTES + MORSECODE_WORD_INDEX_BYTES);

  // SEGENV.aux1 stores: [bit 15: check2] [bit 14: check3] [bits 10-13: text hash (4 bits)] [bits 0-9: pattern length]
  bool lastCheck2 = (SEGENV.aux1 & 0x8000) != 0;
  bool lastCheck3 = (SEGENV.aux1 & 0x4000) != 0;
  uint16_t lastHashBits = (SEGENV.aux1 >> 10) & 0xF; // 4 bits of hash
  uint16_t patternLength = SEGENV.aux1 & 0x3FF; // Lower 10 bits for length (up to 1023)

  // Compute text hash
  uint16_t textHash = 0;
  for (char *p = text; *p; p++) {
    textHash = ((textHash << 5) + textHash) + *p;
  }
  uint16_t currentHashBits = (textHash >> 12) & 0xF; // Use upper 4 bits of hash

  bool textChanged = (currentHashBits != lastHashBits) && (SEGENV.call > 0);

  // Check if we need to rebuild the pattern
  bool needsRebuild = (SEGENV.call == 0) || textChanged || (SEGMENT.check2 != lastCheck2) || (SEGMENT.check3 != lastCheck3);

  // Initialize on first call or rebuild pattern
  if (needsRebuild) {
    patternLength = 0;

    // Clear the bit array and word index array first
    memset(morsecodePattern, 0, MORSECODE_PATTERN_BYTES);
    memset(wordIndexArray, 0, MORSECODE_WORD_INDEX_BYTES);

    // Track current word index
    uint8_t currentWordIndex = 0;

    // Build complete morse code pattern
    for (char *c = text; *c; c++) {
      if (patternLength >= MORSECODE_MAX_PATTERN_SIZE - 10) break;

      if (*c >= 'A' && *c <= 'Z') {
        build_morsecode_pattern(letters[*c - 'A'], morsecodePattern, wordIndexArray, patternLength, currentWordIndex, MORSECODE_MAX_PATTERN_SIZE);
      }
      else if (*c >= '0' && *c <= '9') {
        build_morsecode_pattern(numbers[*c - '0'], morsecodePattern, wordIndexArray, patternLength, currentWordIndex, MORSECODE_MAX_PATTERN_SIZE);
      }
      else if (*c == ' ') {
        // Space between words - increment word index for next word
        currentWordIndex++;
        // Add 4 additional spaces (7 total with the 3 after each letter)
        for (int x = 0; x < 4; x++) {
          if (patternLength >= MORSECODE_MAX_PATTERN_SIZE) break;
          wordIndexArray[patternLength] = currentWordIndex;
          patternLength++;
        }
      }
      else if (SEGMENT.check2) {
        const char *punctuationCode = nullptr;
        for (const auto& p : punctuation) {
          if (*c == p.character) {
            punctuationCode = p.code;
            break;
          }
        }
        if (punctuationCode) {
          build_morsecode_pattern(punctuationCode, morsecodePattern, wordIndexArray, patternLength, currentWordIndex, MORSECODE_MAX_PATTERN_SIZE);
        }
      }
    }

    if (SEGMENT.check3) {
      build_morsecode_pattern(".-.-.", morsecodePattern, wordIndexArray, patternLength, currentWordIndex, MORSECODE_MAX_PATTERN_SIZE);
    }

    for (int x = 0; x < 7; x++) {
      if (patternLength >= MORSECODE_MAX_PATTERN_SIZE) break;
      wordIndexArray[patternLength] = currentWordIndex;
      patternLength++;
    }

    // Store the total number of words (currentWordIndex + 1 because it's 0-indexed)
    *wordCountPtr = currentWordIndex + 1;

    // Store pattern length, checkbox states, and hash bits in aux1
    SEGENV.aux1 = patternLength | (currentHashBits << 10) | (SEGMENT.check2 ? 0x8000 : 0) | (SEGMENT.check3 ? 0x4000 : 0);

    // Reset the scroll offset
    SEGENV.aux0 = 0;
  }

  // if pattern is empty for some reason, display black background only
  if (patternLength == 0) {
    SEGMENT.fill(BLACK);
    return;
  }

  // Update offset to make the morse code scroll
  // Use step for scroll timing only
  uint32_t cycleTime = 50 + (255 - SEGMENT.speed)*3;
  uint32_t it = strip.now / cycleTime;
  if (SEGENV.step != it) {
    SEGENV.aux0++;
    SEGENV.step = it;
  }

  // Clear background
  SEGMENT.fill(BLACK);

  // Draw the scrolling pattern
  int offset = SEGENV.aux0 % patternLength;

  // Get the word count and calculate color spacing
  uint8_t wordCount = *wordCountPtr;
  if (wordCount == 0) wordCount = 1;
  uint8_t colorSpacing = 255 / wordCount; // Distribute colors evenly across color wheel/palette

  for (int i = 0; i < SEGLEN; i++) {
    int patternIndex = (offset + i) % patternLength;
    if (GET_BIT8(morsecodePattern, patternIndex)) {
      uint8_t wordIdx = wordIndexArray[patternIndex];
      if (SEGMENT.check1) {  // make each word a separate color
        if (SEGMENT.custom3 < 16)
          // use word index to select base color, add slight offset for animation
          SEGMENT.setPixelColor(i, SEGMENT.color_wheel((wordIdx * colorSpacing) + (SEGENV.aux0 / 4)));
        else
          SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(wordIdx * colorSpacing, true, PALETTE_SOLID_WRAP, 0));
      }
      else {
        if (SEGMENT.custom3 < 16)
          SEGMENT.setPixelColor(i, SEGMENT.color_wheel(SEGENV.aux0 + i));
        else
          SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 0));
      }
    }
  }
}
static const char _data_FX_MODE_MORSECODE[] PROGMEM = "Morse Code@Speed,,,,Color mode,Color by Word,Punctuation,EndOfMessage;;!;1;sx=192,c3=8,o1=1,o2=1";

// AI: below section was generated by an AI
namespace {
  enum UfxArtMode : uint8_t {
    UFX_CENTER_FEATHER_DRIFT = 0,
    UFX_RADIAL_BREATH_FIELD,
    UFX_PHASE_WAVE_BLOOM,
    UFX_GUIDED_BREATH_MANDALA,
    UFX_LIQUID_PALETTE_DRIFT,
    UFX_CAUSTIC_LIGHT_FIELD,
    UFX_AURIC_SHELL,
    UFX_CHROMATIC_TIDE,
    UFX_RD_FIELD,
    UFX_POLAR_VORTEX,
    UFX_TIME_LOCKED_CEREMONY,
    UFX_MASSLESS_VEIL,
    UFX_DATA_BLOOM,
    UFX_ATMOSPHERIC_CAUSTICS,
    UFX_MEMORY_FOG,
    UFX_RESONANCE_FIELD,
    UFX_SONIC_DETONATION,
    UFX_PLASMA_STORM,
    UFX_AUDIO_BLACK_HOLE,
    UFX_SPECTRAL_FIRESTORM,
    UFX_CRYSTAL_SHATTER_FIELD,
    UFX_NEBULA_CANNON,
    UFX_REACTOR_CORE,
    UFX_HYPERDRIVE_TUNNEL,
    UFX_AUDIO_VORTEX,
    UFX_PULSE_PRISM
  };

  struct UfxAudio {
    float volume;
    uint8_t bass;
    uint8_t mids;
    uint8_t highs;
    uint8_t peak;
  };

  inline float ufxClamp01(float v) { return (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v); }
  inline float ufxSmooth(float e0, float e1, float x) {
    float t = ufxClamp01((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
  }
  inline float ufxLerp(float a, float b, float t) { return a + (b - a) * t; }

  UfxAudio ufxReadAudio() {
    UfxAudio a = {0, 0, 0, 0, 0};
    um_data_t *um_data = nullptr;
    if (!UsermodManager::getUMData(&um_data, USERMOD_ID_AUDIOREACTIVE) || !um_data) return a;
    float volumeSmth = *(float*)um_data->u_data[0];
    uint8_t *fft = (uint8_t*)um_data->u_data[2];
    uint8_t peak = *(uint8_t*)um_data->u_data[3];
    a.volume = ufxClamp01(volumeSmth / 255.0f);
    if (fft) {
      a.bass = (uint8_t)((fft[0] + fft[1] + fft[2]) / 3);
      a.mids = (uint8_t)((fft[4] + fft[6] + fft[8]) / 3);
      a.highs = (uint8_t)((fft[12] + fft[14] + fft[15]) / 3);
    }
    a.peak = peak;
    return a;
  }

  void mode_2D_art_suite(uint8_t mode, bool audioReactive) {
    if (!strip.isMatrix || !SEGMENT.is2D()) FX_FALLBACK_STATIC;

    const float w = (SEG_W > 1) ? (float)(SEG_W - 1) : 1.0f;
    const float h = (SEG_H > 1) ? (float)(SEG_H - 1) : 1.0f;
    const float cx = w * 0.5f;
    const float cy = h * 0.5f;
    const float norm = sqrtf(cx * cx + cy * cy) + 0.0001f;

    const float speed = 0.02f + ((float)SEGMENT.speed / 255.0f) * 1.75f;
    const float intensity = (float)SEGMENT.intensity / 255.0f;
    const float c1 = (float)SEGMENT.custom1 / 255.0f;
    const float c2 = (float)SEGMENT.custom2 / 255.0f;
    const float c3 = (float)SEGMENT.custom3 / 31.0f; // 5-bit slider in user_fx

    float cycleMinutes = 0.2f + c1 * 60.0f;
    if (mode == UFX_CENTER_FEATHER_DRIFT || mode == UFX_LIQUID_PALETTE_DRIFT || mode == UFX_TIME_LOCKED_CEREMONY) cycleMinutes = 2.0f + c1 * 58.0f;

    const float phase = ((float)strip.now * (0.001f * speed)) / (cycleMinutes * 60.0f);
    const float p2pi = phase * (2.0f * PI);
    const UfxAudio aud = audioReactive ? ufxReadAudio() : UfxAudio{0, 0, 0, 0, 0};

    for (int y = 0; y < SEG_H; y++) {
      for (int x = 0; x < SEG_W; x++) {
        float dx = x - cx;
        float dy = y - cy;
        float r = sqrtf(dx * dx + dy * dy) / norm;
        float ang = atan2_t(dy, dx);
        float nx = x / w;
        float ny = y / h;
        float field = 0.0f;
        float bloom = 1.0f;

        switch (mode) {
          case UFX_CENTER_FEATHER_DRIFT: {
            float feather = ufxLerp(0.03f, 0.65f, c2);
            float centerBias = ufxLerp(0.7f, 2.6f, c3);
            float edgeLag = SEGMENT.check1 ? 1.8f : 1.0f;
            float shaped = powf(ufxClamp01(1.0f - r), centerBias);
            float lag = (1.0f - shaped) * edgeLag;
            float wave = sin_t((p2pi * 0.15f) - (lag * (0.6f + intensity * 1.8f)) + (ang * 0.1f));
            field = ufxSmooth(0.0f, feather + 0.02f, shaped) * 0.85f + (wave * 0.15f + 0.15f);
            bloom = ufxLerp(0.75f, 1.0f, ufxSmooth(0.0f, 1.0f, shaped));
            break;
          }
          case UFX_RADIAL_BREATH_FIELD: {
            float breath = SEGMENT.check1 ? 0.5f : 1.0f;
            float pulse = sin_t((p2pi * breath) - r * ufxLerp(2.0f, 8.0f, c2));
            float core = powf(ufxClamp01(1.0f - r), ufxLerp(0.6f, 2.4f, c3));
            field = (pulse * 0.5f + 0.5f) * 0.35f + core * 0.65f;
            break;
          }
          case UFX_PHASE_WAVE_BLOOM:
            field = sin_t((p2pi * 0.7f) + (ang * ufxLerp(0.5f, 6.0f, c1)) - (r * ufxLerp(1.0f, 12.0f, c2))) * 0.5f + 0.5f;
            bloom = ufxLerp(0.6f, 1.0f, ufxSmooth(0.1f, 0.9f, 1.0f - r));
            break;
          case UFX_GUIDED_BREATH_MANDALA:
            field = sin_t((p2pi * 0.55f) + r * ufxLerp(6.0f, 30.0f, c1) + sin_t(ang * ufxLerp(1.0f, 6.0f, c3))) * 0.5f + 0.5f;
            field = ufxSmooth(0.05f, ufxLerp(0.35f, 0.95f, c2), field);
            break;
          case UFX_LIQUID_PALETTE_DRIFT:
            field = (sin_t((p2pi * 0.12f) + nx * 1.6f - ny * 1.2f) + sin_t((p2pi * 0.09f) - nx * 0.9f + r * 2.2f)) * 0.25f + 0.5f;
            field = ufxLerp(field, powf(ufxClamp01(1.0f - r), ufxLerp(0.8f, 2.1f, c2)), c3 * 0.45f);
            break;
          case UFX_CAUSTIC_LIGHT_FIELD:
            field = (sin_t((p2pi * 0.8f) + nx * 8.0f + ny * 3.0f) + sin_t((p2pi * 1.05f) + ny * 7.0f - nx * 2.0f)) * 0.25f + 0.5f;
            field = ufxSmooth(0.2f, 0.95f, field);
            break;
          case UFX_AURIC_SHELL:
            field = sin_t((p2pi * 0.25f) + r * ufxLerp(10.0f, 40.0f, c1));
            field = ufxSmooth(0.45f, 0.95f, field * 0.5f + 0.5f) * ufxSmooth(0.0f, 1.0f, 1.0f - r * ufxLerp(0.5f, 1.1f, c3));
            break;
          case UFX_CHROMATIC_TIDE: {
            float dir = SEGMENT.check1 ? ny : nx;
            if (SEGMENT.check2) dir = nx * 0.6f + ny * 0.4f;
            if (SEGMENT.check3) dir = (ang + PI) / (2 * PI);
            field = sin_t((p2pi * ufxLerp(0.2f, 1.2f, c1)) + dir * ufxLerp(2.0f, 12.0f, c2)) * 0.5f + 0.5f;
            break;
          }
          case UFX_RD_FIELD:
            field = perlin8((uint32_t)((nx * (30 + c1 * 120) + phase * 14.0f) * 256.0f), (uint32_t)((ny * (30 + c2 * 120) - phase * 10.0f) * 256.0f)) / 255.0f;
            field = ufxSmooth(0.18f, 0.88f, field);
            break;
          case UFX_POLAR_VORTEX:
            field = sin_t((p2pi * ufxLerp(0.2f, 1.4f, c1)) + (ang * ufxLerp(1.0f, 8.0f, c2)) - (r * ufxLerp(2.0f, 12.0f, c3))) * 0.5f + 0.5f;
            break;
          case UFX_TIME_LOCKED_CEREMONY: {
            float p = fmodf(phase, 1.0f);
            float arrival = ufxSmooth(0.0f, 0.20f, p);
            float build = ufxSmooth(0.18f, 0.45f, p);
            float still = 1.0f - ufxSmooth(0.52f, 0.72f, p);
            float release = ufxSmooth(0.72f, 1.0f, p);
            field = (arrival * (1.0f - r) + build * sin_t(r * 8.0f + p2pi) * 0.5f + 0.5f * still + release * r) * 0.8f;
            break;
          }
          case UFX_MASSLESS_VEIL:
            field = (sin_t(p2pi * 0.24f + nx * 6.0f) + sin_t(p2pi * 0.19f - ny * 5.0f) + sin_t(p2pi * 0.12f + (nx + ny) * 4.0f)) / 6.0f + 0.5f;
            break;
          case UFX_DATA_BLOOM:
            field = ufxSmooth(0.35f, 0.9f, perlin8((uint32_t)((nx * 70 + phase * 26) * 256), (uint32_t)((ny * 70 - phase * 14) * 256)) / 255.0f);
            break;
          case UFX_ATMOSPHERIC_CAUSTICS:
            field = (sin_t(p2pi * 0.33f + nx * 4.0f + ny * 2.0f) + sin_t(p2pi * 0.41f + ny * 6.0f - nx * 1.2f)) * 0.25f + 0.5f;
            field = ufxLerp(field, 1.0f - r, c3 * 0.35f);
            break;
          case UFX_MEMORY_FOG:
            field = perlin8((uint32_t)((nx * 44 + phase * 6.0f) * 256), (uint32_t)((ny * 44 + phase * 4.0f) * 256)) / 255.0f;
            field = ufxSmooth(0.12f, 0.82f, field) * ufxLerp(1.0f, 1.0f - r, c3 * 0.5f);
            break;
          case UFX_RESONANCE_FIELD:
            field = sin_t((p2pi * ufxLerp(0.4f, 1.8f, c2)) + (r * ufxLerp(8.0f, 36.0f, c1)) + (ang * ufxLerp(0.0f, 2.0f, c3))) * 0.5f + 0.5f;
            break;
          case UFX_SONIC_DETONATION:
            field = sin_t((p2pi * 0.8f) - r * (5.0f + aud.bass / 25.0f) + ang * (SEGMENT.check1 ? 1.2f : 0.0f)) * 0.5f + 0.5f;
            field *= ufxLerp(0.35f, 1.0f, aud.bass / 255.0f);
            break;
          case UFX_PLASMA_STORM:
            field = (sin_t((p2pi * 1.4f) + nx * 7.0f + aud.highs * 0.02f) + sin_t((p2pi * 1.1f) + ny * 9.0f - aud.mids * 0.02f)) * 0.25f + 0.5f;
            break;
          case UFX_AUDIO_BLACK_HOLE:
            field = sin_t((p2pi * 0.6f) + ang * ufxLerp(1.0f, 8.0f, c2) - r * (2.0f + aud.bass / 32.0f)) * 0.5f + 0.5f;
            field *= ufxSmooth(0.0f, 1.0f, 1.0f - r * ufxLerp(0.8f, 1.6f, c1));
            break;
          case UFX_SPECTRAL_FIRESTORM:
            field = perlin8((uint32_t)((nx * 55 + phase * 14) * 256), (uint32_t)(((1.0f - ny) * 90 + phase * 20 + aud.bass * 0.1f) * 256)) / 255.0f;
            field = ufxSmooth(0.18f, 0.96f, field);
            break;
          case UFX_CRYSTAL_SHATTER_FIELD:
            field = sin_t((p2pi * 1.2f) + (fabsf(dx) + fabsf(dy)) * ufxLerp(0.5f, 2.8f, c1)) * 0.5f + 0.5f;
            field = ufxLerp(field, ufxSmooth(0.4f, 0.95f, field), aud.peak ? 0.7f : 0.25f);
            break;
          case UFX_NEBULA_CANNON:
            field = perlin8((uint32_t)((nx * 35 + phase * (8 + aud.highs * 0.04f)) * 256), (uint32_t)((ny * 35 - phase * (6 + aud.mids * 0.04f)) * 256)) / 255.0f;
            field = ufxSmooth(0.2f, 0.9f, field);
            break;
          case UFX_REACTOR_CORE:
            field = (1.0f - r) * ufxLerp(0.6f, 1.2f, aud.bass / 255.0f);
            field += (sin_t((p2pi * 1.6f) + r * 16.0f) * 0.5f + 0.5f) * ufxLerp(0.15f, 0.6f, aud.mids / 255.0f);
            break;
          case UFX_HYPERDRIVE_TUNNEL:
            field = sin_t((p2pi * ufxLerp(0.5f, 2.0f, c1 + aud.volume * 0.5f)) + r * ufxLerp(12.0f, 46.0f, c2) + ang * 0.5f) * 0.5f + 0.5f;
            break;
          case UFX_AUDIO_VORTEX:
            field = sin_t((p2pi * ufxLerp(0.3f, 2.0f, c2 + aud.volume * 0.4f)) + ang * ufxLerp(1.0f, 12.0f, c1) - r * ufxLerp(2.0f, 10.0f, c3)) * 0.5f + 0.5f;
            break;
          case UFX_PULSE_PRISM: {
            float poly = 3.0f + c1 * 9.0f;
            float a = floorf(((ang + PI) / (2 * PI)) * poly) / poly;
            field = sin_t((p2pi * 1.0f) + a * (2 * PI) + r * 8.0f) * 0.5f + 0.5f;
            field = ufxLerp(field, 1.0f, aud.peak ? 0.5f : 0.0f);
            break;
          }
        }

        field = ufxClamp01(field);
        uint8_t palIndex = (uint8_t)(255.0f * ufxClamp01(field * ufxLerp(0.65f, 1.35f, intensity) + phase * 0.8f - r * c2 * 0.75f));
        uint8_t bri = (uint8_t)(255.0f * ufxClamp01((0.08f + field * 0.92f) * bloom));
        SEGMENT.setPixelColorXY(x, y, ColorFromPalette(SEGPALETTE, palIndex, bri, LINEARBLEND));
      }
    }

    uint8_t blur = 32 + (SEGMENT.intensity >> 1);
    if (SEGMENT.check2) blur = qadd8(blur, 32);
    SEGMENT.blur(blur, SEGMENT.check3);
  }
}

static void mode_center_feather_drift_ufx(void) { mode_2D_art_suite(UFX_CENTER_FEATHER_DRIFT, false); }
static void mode_radial_breath_field_ufx(void) { mode_2D_art_suite(UFX_RADIAL_BREATH_FIELD, false); }
static void mode_phase_wave_bloom_ufx(void) { mode_2D_art_suite(UFX_PHASE_WAVE_BLOOM, false); }
static void mode_guided_breath_mandala_ufx(void) { mode_2D_art_suite(UFX_GUIDED_BREATH_MANDALA, false); }
static void mode_liquid_palette_drift_ufx(void) { mode_2D_art_suite(UFX_LIQUID_PALETTE_DRIFT, false); }
static void mode_caustic_light_field_ufx(void) { mode_2D_art_suite(UFX_CAUSTIC_LIGHT_FIELD, false); }
static void mode_auric_shell_ufx(void) { mode_2D_art_suite(UFX_AURIC_SHELL, false); }
static void mode_chromatic_tide_ufx(void) { mode_2D_art_suite(UFX_CHROMATIC_TIDE, false); }
static void mode_rd_field_ufx(void) { mode_2D_art_suite(UFX_RD_FIELD, false); }
static void mode_polar_vortex_ufx(void) { mode_2D_art_suite(UFX_POLAR_VORTEX, false); }
static void mode_time_locked_ceremony_ufx(void) { mode_2D_art_suite(UFX_TIME_LOCKED_CEREMONY, false); }
static void mode_massless_veil_ufx(void) { mode_2D_art_suite(UFX_MASSLESS_VEIL, false); }
static void mode_data_bloom_ufx(void) { mode_2D_art_suite(UFX_DATA_BLOOM, false); }
static void mode_atmospheric_caustics_ufx(void) { mode_2D_art_suite(UFX_ATMOSPHERIC_CAUSTICS, false); }
static void mode_memory_fog_ufx(void) { mode_2D_art_suite(UFX_MEMORY_FOG, false); }
static void mode_resonance_field_ufx(void) { mode_2D_art_suite(UFX_RESONANCE_FIELD, false); }
static void mode_sonic_detonation_ufx(void) { mode_2D_art_suite(UFX_SONIC_DETONATION, true); }
static void mode_plasma_storm_ufx(void) { mode_2D_art_suite(UFX_PLASMA_STORM, true); }
static void mode_audio_black_hole_ufx(void) { mode_2D_art_suite(UFX_AUDIO_BLACK_HOLE, true); }
static void mode_spectral_firestorm_ufx(void) { mode_2D_art_suite(UFX_SPECTRAL_FIRESTORM, true); }
static void mode_crystal_shatter_field_ufx(void) { mode_2D_art_suite(UFX_CRYSTAL_SHATTER_FIELD, true); }
static void mode_nebula_cannon_ufx(void) { mode_2D_art_suite(UFX_NEBULA_CANNON, true); }
static void mode_reactor_core_ufx(void) { mode_2D_art_suite(UFX_REACTOR_CORE, true); }
static void mode_hyperdrive_tunnel_ufx(void) { mode_2D_art_suite(UFX_HYPERDRIVE_TUNNEL, true); }
static void mode_audio_vortex_ufx(void) { mode_2D_art_suite(UFX_AUDIO_VORTEX, true); }
static void mode_pulse_prism_ufx(void) { mode_2D_art_suite(UFX_PULSE_PRISM, true); }

static const char _data_FX_MODE_CENTER_FEATHER_DRIFT_UFX[] PROGMEM = "Center Feather Drift@!,!,Cycle Length,Feather Width,Center Bias,Edge Lag,Soft Blur,Directional Drift;;!;2;pal=54,sx=14,ix=96,c1=120,c2=200,c3=21,o1=1,o2=0,o3=1";
static const char _data_FX_MODE_RADIAL_BREATH_FIELD_UFX[] PROGMEM = "Radial Breath Field@!,!,Breath Length,Feather Width,Center Size,Drift,Symmetric,Slow Core,Smear;;!;2;pal=71,sx=30,ix=96,c1=40,c2=170,c3=18,o1=1,o2=0,o3=1";
static const char _data_FX_MODE_PHASE_WAVE_BLOOM_UFX[] PROGMEM = "Phase-Wave Bloom@!,!,Angular Influence,Radial Spread,Feather,Bloom,Alt Harmonic,Soft Blur,Smear;;!;2;pal=11,sx=44,ix=128,c1=128,c2=110,c3=21,o1=0,o2=1,o3=1";
static const char _data_FX_MODE_GUIDED_BREATH_MANDALA_UFX[] PROGMEM = "Guided Breathing Mandala@!,!,Ring Density,Softness,Center Glow,Asymmetry,Alt Rings,Calm Blend,Smear;;!;2;pal=16,sx=24,ix=110,c1=100,c2=170,c3=10,o1=1,o2=1,o3=1";
static const char _data_FX_MODE_LIQUID_PALETTE_DRIFT_UFX[] PROGMEM = "Liquid Palette Drift@!,!,Cycle Length,Center Bias,Edge Lag,Softness,Calm,Wide Flow,Smear;;!;2;pal=50,sx=16,ix=82,c1=180,c2=160,c3=17,o1=1,o2=1,o3=1";
static const char _data_FX_MODE_CAUSTIC_LIGHT_FIELD_UFX[] PROGMEM = "Caustic Light Field@!,!,Wave Scale,Refraction,Softness,Drift,Interfere,Wide Blur,Smear;;!;2;pal=70,sx=88,ix=132,c1=100,c2=130,c3=20,o1=1,o2=0,o3=1";
static const char _data_FX_MODE_AURIC_SHELL_UFX[] PROGMEM = "Auric Shell@!,!,Shell Density,Softness,Pulse Depth,Separation,Alt Shells,Center Lock,Smear;;!;2;pal=45,sx=60,ix=120,c1=110,c2=170,c3=13,o1=0,o2=1,o3=1";
static const char _data_FX_MODE_CHROMATIC_TIDE_UFX[] PROGMEM = "Chromatic Tide@!,!,Direction Mode,Width,Softness,Contrast,Vertical,Diagonal,Rotational;;!;2;pal=63,sx=110,ix=128,c1=70,c2=140,c3=22,o1=0,o2=0,o3=0";
static const char _data_FX_MODE_RD_FIELD_UFX[] PROGMEM = "Reaction-Diffusion Field@!,!,Scale,Diffusion,Smoothing,Contrast,Organic,Dense,Smear;;!;2;pal=67,sx=72,ix=128,c1=110,c2=100,c3=22,o1=1,o2=1,o3=1";
static const char _data_FX_MODE_POLAR_VORTEX_UFX[] PROGMEM = "Polar Vortex / Galaxy Drift@!,!,Swirl,Core Pull,Ring Breakup,Bloom,Clockwise,Tight Core,Smear;;!;2;pal=27,sx=68,ix=124,c1=120,c2=140,c3=11,o1=1,o2=1,o3=1";
static const char _data_FX_MODE_TIME_LOCKED_CEREMONY_UFX[] PROGMEM = "Time-Locked Ceremony@!,!,Cycle Length,Stillness,Phase Contrast,Center Bias,Ceremony,Long Hold,Smear;;!;2;pal=24,sx=12,ix=96,c1=170,c2=130,c3=20,o1=1,o2=1,o3=1";
static const char _data_FX_MODE_MASSLESS_VEIL_UFX[] PROGMEM = "Massless Veil@!,!,Layering,Softness,Depth Separation,Turbulence,Slow Layers,Wide Curtains,Smear;;!;2;pal=62,sx=40,ix=90,c1=120,c2=180,c3=13,o1=1,o2=1,o3=1";
static const char _data_FX_MODE_DATA_BLOOM_UFX[] PROGMEM = "Data Bloom@!,!,Density,Growth,Decay,Softness,Cluster Bias,Glow,Smear;;!;2;pal=65,sx=72,ix=128,c1=140,c2=130,c3=20,o1=1,o2=1,o3=1";
static const char _data_FX_MODE_ATMOSPHERIC_CAUSTICS_UFX[] PROGMEM = "Atmospheric Caustics@!,!,Flow Scale,Refraction,Softness,Brightness Floor,Open Flow,Airy,Smear;;!;2;pal=71,sx=46,ix=110,c1=90,c2=120,c3=23,o1=1,o2=1,o3=1";
static const char _data_FX_MODE_MEMORY_FOG_UFX[] PROGMEM = "Memory Fog@!,!,Density,Dissolve,Center Pull,Contrast,Fog Bank,Low Motion,Smear;;!;2;pal=47,sx=18,ix=86,c1=130,c2=170,c3=18,o1=1,o2=1,o3=1";
static const char _data_FX_MODE_RESONANCE_FIELD_UFX[] PROGMEM = "Resonance Field@!,!,Resonance Width,Pulse Interval,Asymmetry,Bloom,Harmonic,Slow Pulse,Smear;;!;2;pal=20,sx=82,ix=130,c1=100,c2=140,c3=9,o1=1,o2=0,o3=1";
static const char _data_FX_MODE_SONIC_DETONATION_UFX[] PROGMEM = "Sonic Detonation@!,!,Impact Size,Distortion,Crackle,Decay,Center Burst,Warp,Smear;;!;2f;pal=0,sx=128,ix=170,c1=110,c2=120,c3=16,o1=1,o2=1,o3=1";
static const char _data_FX_MODE_PLASMA_STORM_UFX[] PROGMEM = "Plasma Storm@!,!,Arc Density,Turbulence,Sharpness,Bloom,Lightning,Shock,Smear;;!;2f;pal=15,sx=144,ix=180,c1=120,c2=150,c3=20,o1=1,o2=1,o3=1";
static const char _data_FX_MODE_AUDIO_BLACK_HOLE_UFX[] PROGMEM = "Audio Black Hole@!,!,Gravity,Orbit,Eruption,Swirl,Orbit Lock,Center Crush,Smear;;!;2f;pal=0,sx=118,ix=168,c1=150,c2=130,c3=13,o1=1,o2=1,o3=1";
static const char _data_FX_MODE_SPECTRAL_FIRESTORM_UFX[] PROGMEM = "Spectral Firestorm@!,!,Flame Height,Turbulence,Sparks,Heat Contrast,Sparks,Plasma,Smear;;!;2f;pal=35,sx=136,ix=180,c1=160,c2=140,c3=18,o1=1,o2=1,o3=1";
static const char _data_FX_MODE_CRYSTAL_SHATTER_FIELD_UFX[] PROGMEM = "Crystal Shatter Field@!,!,Fracture Size,Propagation,Rebuild,Prism,Hits Hold,Shard Glow,Smear;;!;2f;pal=66,sx=126,ix=170,c1=100,c2=120,c3=24,o1=1,o2=1,o3=1";
static const char _data_FX_MODE_NEBULA_CANNON_UFX[] PROGMEM = "Nebula Cannon@!,!,Cloud Density,Expansion,Turbulence,Persistence,Burst,Long Trail,Smear;;!;2f;pal=54,sx=122,ix=165,c1=130,c2=120,c3=23,o1=1,o2=1,o3=1";
static const char _data_FX_MODE_REACTOR_CORE_UFX[] PROGMEM = "Reactor Core@!,!,Core Size,Build Rate,Venting,Instability,Core Lock,Ring Vent,Smear;;!;2f;pal=14,sx=132,ix=175,c1=140,c2=150,c3=15,o1=1,o2=1,o3=1";
static const char _data_FX_MODE_HYPERDRIVE_TUNNEL_UFX[] PROGMEM = "Hyperdrive Tunnel@!,!,Depth,Streak Density,Perspective Warp,Glow,Forward,Warp,Smear;;!;2f;pal=61,sx=146,ix=185,c1=140,c2=150,c3=21,o1=1,o2=1,o3=1";
static const char _data_FX_MODE_AUDIO_VORTEX_UFX[] PROGMEM = "Audio Vortex@!,!,Width,Spin,Compression,Turbulence,Clockwise,Tighten,Smear;;!;2f;pal=28,sx=138,ix=176,c1=120,c2=150,c3=15,o1=1,o2=1,o3=1";
static const char _data_FX_MODE_PULSE_PRISM_UFX[] PROGMEM = "Pulse Prism@!,!,Polygon Count,Pulse Size,Prism Strength,Decay,Angular Lock,Peak Hold,Smear;;!;2f;pal=18,sx=134,ix=170,c1=90,c2=130,c3=23,o1=1,o2=1,o3=1";
// AI: end


/////////////////////
//  UserMod Class  //
/////////////////////

class UserFxUsermod : public Usermod {
 private:
 public:
  void setup() override {
    strip.addEffect(255, &mode_diffusionfire, _data_FX_MODE_DIFFUSIONFIRE);
    strip.addEffect(255, &mode_spinning_wheel, _data_FX_MODE_SPINNINGWHEEL);
    strip.addEffect(255, &mode_2D_lavalamp, _data_FX_MODE_2D_LAVALAMP);
    strip.addEffect(255, &mode_2D_magma, _data_FX_MODE_2D_MAGMA);
    strip.addEffect(255, &mode_ants, _data_FX_MODE_ANTS);
    strip.addEffect(255, &mode_morsecode, _data_FX_MODE_MORSECODE);

    strip.addEffect(255, &mode_center_feather_drift_ufx, _data_FX_MODE_CENTER_FEATHER_DRIFT_UFX);
    strip.addEffect(255, &mode_radial_breath_field_ufx, _data_FX_MODE_RADIAL_BREATH_FIELD_UFX);
    strip.addEffect(255, &mode_phase_wave_bloom_ufx, _data_FX_MODE_PHASE_WAVE_BLOOM_UFX);
    strip.addEffect(255, &mode_guided_breath_mandala_ufx, _data_FX_MODE_GUIDED_BREATH_MANDALA_UFX);
    strip.addEffect(255, &mode_liquid_palette_drift_ufx, _data_FX_MODE_LIQUID_PALETTE_DRIFT_UFX);
    strip.addEffect(255, &mode_caustic_light_field_ufx, _data_FX_MODE_CAUSTIC_LIGHT_FIELD_UFX);
    strip.addEffect(255, &mode_auric_shell_ufx, _data_FX_MODE_AURIC_SHELL_UFX);
    strip.addEffect(255, &mode_chromatic_tide_ufx, _data_FX_MODE_CHROMATIC_TIDE_UFX);
    strip.addEffect(255, &mode_rd_field_ufx, _data_FX_MODE_RD_FIELD_UFX);
    strip.addEffect(255, &mode_polar_vortex_ufx, _data_FX_MODE_POLAR_VORTEX_UFX);
    strip.addEffect(255, &mode_time_locked_ceremony_ufx, _data_FX_MODE_TIME_LOCKED_CEREMONY_UFX);
    strip.addEffect(255, &mode_massless_veil_ufx, _data_FX_MODE_MASSLESS_VEIL_UFX);
    strip.addEffect(255, &mode_data_bloom_ufx, _data_FX_MODE_DATA_BLOOM_UFX);
    strip.addEffect(255, &mode_atmospheric_caustics_ufx, _data_FX_MODE_ATMOSPHERIC_CAUSTICS_UFX);
    strip.addEffect(255, &mode_memory_fog_ufx, _data_FX_MODE_MEMORY_FOG_UFX);
    strip.addEffect(255, &mode_resonance_field_ufx, _data_FX_MODE_RESONANCE_FIELD_UFX);
    strip.addEffect(255, &mode_sonic_detonation_ufx, _data_FX_MODE_SONIC_DETONATION_UFX);
    strip.addEffect(255, &mode_plasma_storm_ufx, _data_FX_MODE_PLASMA_STORM_UFX);
    strip.addEffect(255, &mode_audio_black_hole_ufx, _data_FX_MODE_AUDIO_BLACK_HOLE_UFX);
    strip.addEffect(255, &mode_spectral_firestorm_ufx, _data_FX_MODE_SPECTRAL_FIRESTORM_UFX);
    strip.addEffect(255, &mode_crystal_shatter_field_ufx, _data_FX_MODE_CRYSTAL_SHATTER_FIELD_UFX);
    strip.addEffect(255, &mode_nebula_cannon_ufx, _data_FX_MODE_NEBULA_CANNON_UFX);
    strip.addEffect(255, &mode_reactor_core_ufx, _data_FX_MODE_REACTOR_CORE_UFX);
    strip.addEffect(255, &mode_hyperdrive_tunnel_ufx, _data_FX_MODE_HYPERDRIVE_TUNNEL_UFX);
    strip.addEffect(255, &mode_audio_vortex_ufx, _data_FX_MODE_AUDIO_VORTEX_UFX);
    strip.addEffect(255, &mode_pulse_prism_ufx, _data_FX_MODE_PULSE_PRISM_UFX);

    ////////////////////////////////////////
    //  add your effect function(s) here  //
    ////////////////////////////////////////

    // use id=255 for all custom user FX (the final id is assigned when adding the effect)

    // strip.addEffect(255, &mode_your_effect, _data_FX_MODE_YOUR_EFFECT);
    // strip.addEffect(255, &mode_your_effect2, _data_FX_MODE_YOUR_EFFECT2);
    // strip.addEffect(255, &mode_your_effect3, _data_FX_MODE_YOUR_EFFECT3);
  }


  ///////////////////////////////////////////////////////////////////////////////////////////////
  //  If you want configuration options in the usermod settings page, implement these methods  //
  ///////////////////////////////////////////////////////////////////////////////////////////////

  // void addToConfig(JsonObject& root) override
  // {
  //   JsonObject top = root.createNestedObject(FPSTR("User FX"));
  //   top["myConfigValue"] = myConfigValue;
  // }
  // bool readFromConfig(JsonObject& root) override
  // {
  //   JsonObject top = root[FPSTR("User FX")];
  //   bool configComplete = !top.isNull();
  //   configComplete &= getJsonValue(top["myConfigValue"], myConfigValue);
  //   return configComplete;
  // }

  void loop() override {} // nothing to do in the loop
  uint16_t getId() override { return USERMOD_ID_USER_FX; }
};

static UserFxUsermod user_fx;
REGISTER_USERMOD(user_fx);
