/*
  limits.c - code pertaining to limit-switches and performing the homing cycle
  Part of Grbl

  Copyright (c) 2009-2011 Simen Svale Skogsrud
  Copyright (c) 2012 Sungeun K. Jeon

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/
  
#include "stepper.h"
#include "settings.h"
#include "nuts_bolts.h"
#include "config.h"
#include "spindle_control.h"
#include "motion_control.h"
#include "planner.h"
#include "protocol.h"
#include "limits.h"
#include "report.h"

#include <mk20dx128.h>
#include <pin_config.h>                  
#include <util.h>

#define MICROSECONDS_PER_ACCELERATION_TICK  (1000000/ACCELERATION_TICKS_PER_SECOND)


void limits_init() 
{
  uint32_t config_reg;
  LIMIT_DDR &= ~(LIMITS_MASK | RESET_BIT | FEED_HOLD_BIT | CYCLE_BIT); // Set as input pins

  #ifndef LIMIT_SWITCHES_ACTIVE_HIGH
  config_reg = PULL_UP | MUX_GPIO;
  #else
  config_reg = PULL_DOWN | MUX_GPIO;
  #endif
  
  LIMIT_X_CTRL = config_reg;
  LIMIT_Y_CTRL = config_reg;
  LIMIT_Z_CTRL = config_reg;

  config_reg = PULL_UP | MUX_GPIO | IRQC_FALLING;
  RESET_CTRL = config_reg;
  FEED_HOLD_CTRL = config_reg;
  CYCLE_CTRL = config_reg;

  toggle_hard_limits(true);
}
 
void toggle_hard_limits(uint32_t enable){
  if(enable && bit_istrue(settings.flags,BITFLAG_HARD_LIMIT_ENABLE)){
    LIMIT_X_CTRL = (LIMIT_X_CTRL & ~ IRQC_MASK) | IRQC_EITHER_EDGE;
    LIMIT_X_CTRL = (LIMIT_Z_CTRL & ~ IRQC_MASK) | IRQC_EITHER_EDGE;
    LIMIT_X_CTRL = (LIMIT_Y_CTRL & ~ IRQC_MASK) | IRQC_EITHER_EDGE;
  }else{
    LIMIT_X_CTRL &= ~IRQC_MASK;
    LIMIT_Y_CTRL &= ~IRQC_MASK;
    LIMIT_Z_CTRL &= ~IRQC_MASK;
  }
}

// This is the Limit Pin Change Interrupt, which handles the hard limit feature. A bouncing 
// limit switch can cause a lot of problems, like false readings and multiple interrupt calls.
// If a switch is triggered at all, something bad has happened and treat it as such, regardless
// if a limit switch is being disengaged. It's impossible to reliably tell the state of a 
// bouncing pin without a debouncing method.
// NOTE: Do not attach an e-stop to the limit pins, because this interrupt is disabled during
// homing cycles and will not respond correctly. Upon user request or need, there may be a
// special pinout for an e-stop, but it is generally recommended to just directly connect
// your e-stop switch to the Arduino reset pin, since it is the most correct way to do this.
void portb_isr(void){
  // TODO: This interrupt may be used to manage the homing cycle directly with the main stepper
  // interrupt without adding too much to it. All it would need is some way to stop one axis 
  // when its limit is triggered and continue the others. This may reduce some of the code, but
  // would make Grbl a little harder to read and understand down road. Holding off on this until
  // we move on to new hardware or flash space becomes an issue. If it ain't broke, don't fix it.

  // Ignore limit switches if already in an alarm state or in-process of executing an alarm.
  // When in the alarm state, Grbl should have been reset or will force a reset, so any pending 
  // moves in the planner and serial buffers are all cleared and newly sent blocks will be 
  // locked out until a homing cycle or a kill lock command. Allows the user to disable the hard
  // limit setting if their limits are constantly triggering after a reset and move their axes.
  if(PORTB_ISFR & LIMITS_MASK){
    // We don't care about which pin actually did anything, just that something happened.
    // Seems pointless, but the write resets the flag
    if(LIMIT_X_CTRL & ISF) LIMIT_X_CTRL |= ISF;
    if(LIMIT_Y_CTRL & ISF) LIMIT_Y_CTRL |= ISF;
    if(LIMIT_Z_CTRL & ISF) LIMIT_Z_CTRL |= ISF;
    
    if (sys.state != STATE_ALARM) { 
      if (bit_isfalse(sys.execute,EXEC_ALARM)) {
	mc_reset(); // Initiate system kill.
	sys.execute |= EXEC_CRIT_EVENT; // Indicate hard limit critical event
      }
    }
  }

  if(PORTB_ISFR & INPUT_MASK){
    if(RESET_CTRL & ISF){
      RESET_CTRL |= ISF;
      mc_reset();
    }
    if(FEED_HOLD_CTRL & ISF){
      FEED_HOLD_CTRL |= ISF;
      sys.execute |= EXEC_FEED_HOLD;
    }
    if(CYCLE_CTRL & ISF){
      CYCLE_CTRL |= ISF;
      sys.execute |= EXEC_CYCLE_START;
    }
  }
}


// Moves all specified axes in same specified direction (positive=true, negative=false)
// and at the homing rate. Homing is a special motion case, where there is only an 
// acceleration followed by abrupt asynchronous stops by each axes reaching their limit 
// switch independently. Instead of shoehorning homing cycles into the main stepper 
// algorithm and overcomplicate things, a stripped-down, lite version of the stepper 
// algorithm is written here. This also lets users hack and tune this code freely for
// their own particular needs without affecting the rest of Grbl.
// NOTE: Only the abort runtime command can interrupt this process.
static void homing_cycle(uint32_t cycle_mask, int32_t pos_dir, bool invert_pin, float homing_rate) 
{
  #ifdef LIMIT_SWITCHES_ACTIVE_HIGH
    // When in an active-high switch configuration, invert_pin needs to be adjusted.
    invert_pin = !invert_pin;
  #endif

  // Determine governing axes with finest step resolution per distance for the Bresenham
  // algorithm. This solves the issue when homing multiple axes that have different 
  // resolutions without exceeding system acceleration setting. It doesn't have to be
  // perfect since homing locates machine zero, but should create for a more consistent 
  // and speedy homing routine.
  // NOTE: For each axes enabled, the following calculations assume they physically move 
  // an equal distance over each time step until they hit a limit switch, aka dogleg.
  uint32_t steps[3];
  uint8_t dist = 0;
  clear_vector(steps);
  if (cycle_mask & (1<<X_AXIS)) { 
    dist++;
    steps[X_AXIS] = lround(settings.steps_per_mm[X_AXIS]); 
  }
  if (cycle_mask & (1<<Y_AXIS)) { 
    dist++;
    steps[Y_AXIS] = lround(settings.steps_per_mm[Y_AXIS]); 
  }
  if (cycle_mask & (1<<Z_AXIS)) {
    dist++;
    steps[Z_AXIS] = lround(settings.steps_per_mm[Z_AXIS]);
  }
  uint32_t step_event_count = max(steps[X_AXIS], max(steps[Y_AXIS], steps[Z_AXIS]));  
  
  // To ensure global acceleration is not exceeded, reduce the governing axes nominal rate
  // by adjusting the actual axes distance traveled per step. This is the same procedure
  // used in the main planner to account for distance traveled when moving multiple axes.
  // NOTE: When axis acceleration independence is installed, this will be updated to move
  // all axes at their maximum acceleration and rate.
  float ds = step_event_count/sqrt(dist);

  // Compute the adjusted step rate change with each acceleration tick. (in step/min/acceleration_tick)
  uint32_t delta_rate = ceil( ds*settings.acceleration/(60*ACCELERATION_TICKS_PER_SECOND));
  
  #ifdef HOMING_RATE_ADJUST
    // Adjust homing rate so a multiple axes moves all at the homing rate independently.
    homing_rate *= sqrt(dist); // Eq. only works if axes values are 1 or 0.
  #endif
  
  // Nominal and initial time increment per step. Nominal should always be greater then 3
  // usec, since they are based on the same parameters as the main stepper routine. Initial
  // is based on the MINIMUM_STEPS_PER_MINUTE config. Since homing feed can be very slow,
  // disable acceleration when rates are below MINIMUM_STEPS_PER_MINUTE.
  uint32_t dt_min = lround(1000000*60/(ds*homing_rate)); // Cruising (usec/step)
  uint32_t dt = 1000000*60/MINIMUM_STEPS_PER_MINUTE; // Initial (usec/step)
  if (dt > dt_min) { dt = dt_min; } // Disable acceleration for very slow rates.
      
  // Set default out_bits. 
  uint32_t out_bits0 = settings.invert_mask;
  out_bits0 ^= (settings.homing_dir_mask & DIRECTION_MASK); // Apply homing direction settings
  if (!pos_dir) { out_bits0 ^= DIRECTION_MASK; }   // Invert bits, if negative dir.
  
  // Initialize stepping variables
  int32_t counter_x = -(step_event_count >> 1); // Bresenham counters
  int32_t counter_y = counter_x;
  int32_t counter_z = counter_x;
  uint32_t step_delay = dt;  // Step delay after pulse
  uint32_t step_rate = 0;  // Tracks step rate. Initialized from 0 rate. (in step/min)
  uint32_t trap_counter = MICROSECONDS_PER_ACCELERATION_TICK/2; // Acceleration trapezoid counter
  uint32_t out_bits;
  uint32_t limit_state;
  for(;;) {
  
    // Reset out bits. Both direction and step pins appropriately inverted and set.
    out_bits = out_bits0;
    
    // Get limit pin state.
    limit_state = LIMIT_PORT(DIR);
    if (invert_pin) { limit_state ^= LIMITS_MASK; } // If leaving switch, invert to move.
    
    // Set step pins by Bresenham line algorithm. If limit switch reached, disable and
    // flag for completion.
    if (cycle_mask & (1<<X_AXIS)) {
      counter_x += steps[X_AXIS];
      if (counter_x > 0) {
        if (limit_state & LIMIT_X_BIT) { out_bits ^= STEP_X_BIT; }
        else { cycle_mask &= ~(1<<X_AXIS); }
        counter_x -= step_event_count;
      }
    }
    if (cycle_mask & (1<<Y_AXIS)) {
      counter_y += steps[Y_AXIS];
      if (counter_y > 0) {
        if (limit_state & LIMIT_Y_BIT) { out_bits ^= STEP_Y_BIT; }
        else { cycle_mask &= ~(1<<Y_AXIS); }
        counter_y -= step_event_count;
      }
    }
    if (cycle_mask & (1<<Z_AXIS)) {
      counter_z += steps[Z_AXIS];
      if (counter_z > 0) {
        if (limit_state & LIMIT_Z_BIT) { out_bits ^= STEP_Z_BIT; }
        else { cycle_mask &= ~(1<<Z_AXIS); }
        counter_z -= step_event_count;
      }
    }        
    
    // Check if we are done or for system abort
    if (!(cycle_mask) || (sys.execute & EXEC_RESET)) { return; }
      
    STEPPER_PORT(DOR) = (STEPPER_PORT(DOR) & ~DIRECTION_MASK) | (out_bits & DIRECTION_MASK);
    trigger_pulse(out_bits & STEP_MASK);
    delay_microseconds(step_delay); // Pules rate

    // Track and set the next step delay, if required. This routine uses another Bresenham
    // line algorithm to follow the constant acceleration line in the velocity and time 
    // domain. This is a lite version of the same routine used in the main stepper program.
    if (dt > dt_min) { // Unless cruising, check for time update.
      trap_counter += dt; // Track time passed since last update.
      if (trap_counter > MICROSECONDS_PER_ACCELERATION_TICK) {
        trap_counter -= MICROSECONDS_PER_ACCELERATION_TICK;
        step_rate += delta_rate; // Increment velocity
        dt = (1000000*60)/step_rate; // Compute new time increment
        if (dt < dt_min) {dt = dt_min;}  // If target rate reached, cruise.
        step_delay = dt;
      }
    }
  }
}


void limits_go_home() 
{  
  // Enable only the steppers, not the cycle. Cycle should be inactive/complete.
  st_wake_up();
  
  // Search to engage all axes limit switches at faster homing seek rate.
  homing_cycle(HOMING_SEARCH_CYCLE_0, true, false, settings.homing_seek_rate);  // Search cycle 0
  #ifdef HOMING_SEARCH_CYCLE_1
    homing_cycle(HOMING_SEARCH_CYCLE_1, true, false, settings.homing_seek_rate);  // Search cycle 1
  #endif
  #ifdef HOMING_SEARCH_CYCLE_2
    homing_cycle(HOMING_SEARCH_CYCLE_2, true, false, settings.homing_seek_rate);  // Search cycle 2
  #endif
  delay(settings.homing_debounce_delay); // Delay to debounce signal
    
  // Now in proximity of all limits. Carefully leave and approach switches in multiple cycles
  // to precisely hone in on the machine zero location. Moves at slower homing feed rate.
  int8_t n_cycle = N_HOMING_LOCATE_CYCLE;
  while (n_cycle--) {
    // Leave all switches to release them. After cycles complete, this is machine zero.
    homing_cycle(HOMING_LOCATE_CYCLE, false, true, settings.homing_feed_rate);
    delay(settings.homing_debounce_delay);
    
    if (n_cycle > 0) {
      // Re-approach all switches to re-engage them.
      homing_cycle(HOMING_LOCATE_CYCLE, true, false, settings.homing_feed_rate);
      delay(settings.homing_debounce_delay);
    }
  }

  st_go_idle(); // Call main stepper shutdown routine.  
}
