/*
  stepper.c - stepper motor pulse generation
  Processes block from the queue generated by the planer and pulses
  steppers accordingly via a dynamically adapted timer interrupt.
  Part of LasaurGrbl

  Copyright (c) 2011 Stefan Hechenberger
  Copyright (c) 2009-2011 Simen Svale Skogsrud
  Copyright (c) 2011 Sungeun K. Jeon
  
  Inspired by the 'RepRap cartesian firmware' by Zack Smith and 
  Philipp Tiefenbacher.

  LasaurGrbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  LasaurGrbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  ---
  
           __________________________
          /|                        |\     _________________         ^
         / |                        | \   /|               |\        |
        /  |                        |  \ / |               | \       s
       /   |                        |   |  |               |  \      p
      /    |                        |   |  |               |   \     e
     +-----+------------------------+---+--+---------------+----+    e
     |               BLOCK 1            |      BLOCK 2          |    d
  
                             time ----->

  The speed profile starts at block->initial_rate, accelerates by block->rate_delta
  during the first block->accelerate_until step_events_completed, then keeps going at constant speed until
  step_events_completed reaches block->decelerate_after after which it decelerates until final_rate is reached.
  The slope of acceleration is always +/- block->rate_delta and is applied at a constant rate following the midpoint rule.
  Speed adjustments are made ACCELERATION_TICKS_PER_SECOND times per second.  
*/

#include <math.h>
#include <stdlib.h>
#include <util/delay.h>
#include <avr/interrupt.h>
#include <string.h>
#include "stepper.h"
#include "config.h"
#include "gcode.h"
#include "planner.h"
#include "sense_control.h"
#include "serial.h"  //for debug


#define CYCLES_PER_MICROSECOND (F_CPU/1000000)  //16000000/1000000 = 16
#define CYCLES_PER_ACCELERATION_TICK (F_CPU/ACCELERATION_TICKS_PER_SECOND)  // 24MHz/100 = 240000


static int32_t stepper_position[3];  // real-time position in absolute steps
static block_t *current_block;  // A pointer to the block currently being traced

// Variables used by The Stepper Driver Interrupt
static uint8_t out_bits;       // The next stepping-bits to be output
static int32_t counter_x,       // Counter variables for the bresenham line tracer
               counter_y,
               counter_z;
static uint32_t step_events_completed; // The number of step events executed in the current block
static volatile uint8_t busy;  // true whe stepper ISR is in already running

// Variables used by the trapezoid generation
static uint32_t cycles_per_step_event;        // The number of machine cycles between each step event
static uint32_t acceleration_tick_counter;    // The cycles since last acceleration_tick.
                                              // Used to generate ticks at a steady pace without allocating a separate timer.
static uint32_t adjusted_rate;                // The current rate of step_events according to the speed profile
static bool processing_flag;                  // indicates if blocks are being processed
static volatile bool stop_requested;          // when set to true stepper interrupt will go idle on next entry
static volatile uint8_t stop_status;          // yields the reason for a stop request


// prototypes for static functions (non-accesible from other files)
static bool acceleration_tick();
static void adjust_speed( uint32_t steps_per_minute );
static uint32_t config_step_timer(uint32_t cycles);



// Initialize and start the stepper motor subsystem
void stepper_init() {  
  // Configure directions of interface pins
  STEPPING_DDR |= (STEPPING_MASK | DIRECTION_MASK);
  STEPPING_PORT = (STEPPING_PORT & ~(STEPPING_MASK | DIRECTION_MASK)) | INVERT_MASK;
  
  // waveform generation = 0100 = CTC
  TCCR1B &= ~(1<<WGM13);
  TCCR1B |= (1<<WGM12);
  TCCR1A &= ~(1<<WGM11);
  TCCR1A &= ~(1<<WGM10);

  // output mode = 00 (disconnected)
  TCCR1A &= ~(3<<COM1A0);
  TCCR1A &= ~(3<<COM1B0);

  // Configure Timer 2
  TCCR2A = 0; // Normal operation
  TCCR2B = 0; // Disable timer until needed.
  TIMSK2 |= (1<<TOIE2); // Enable Timer2 interrupt flag
  
  adjust_speed(MINIMUM_STEPS_PER_MINUTE);
  clear_vector(stepper_position);
  acceleration_tick_counter = 0;
  current_block = NULL;
  stop_requested = false;
  stop_status = STATUS_OK;
  busy = false;
  
  // start in the idle state
  // The stepper interrupt gets started when blocks are being added.
  stepper_go_idle();  
}


// block until all command blocks are executed
void stepper_synchronize() {
  while(processing_flag) { 
    sleep_mode();
  }
}


// start processing command blocks
void stepper_wake_up() {
  if (!processing_flag) {
    processing_flag = true;
    // Initialize stepper output bits
    out_bits = INVERT_MASK;
    // Enable stepper driver interrupt
    TIMSK1 |= (1<<OCIE1A);
  }
}


// stop processing command blocks
void stepper_go_idle() {
  processing_flag = false;
  current_block = NULL;
  // Disable stepper driver interrupt
  TIMSK1 &= ~(1<<OCIE1A);
  control_laser_intensity(0);
}

// stop processing command blocks, absorb serial data
void stepper_request_stop(uint8_t status) {
  stop_status = status;
  stop_requested = true;
}

bool stepper_stop_requested() {
  return stop_requested;
}

uint8_t stepper_stop_status() {
  return stop_status;
}

void stepper_resume() {
  stop_requested = false;
}



double stepper_get_position_x() {
  return stepper_position[X_AXIS]/CONFIG_X_STEPS_PER_MM;
}
double stepper_get_position_y() {
  return stepper_position[Y_AXIS]/CONFIG_Y_STEPS_PER_MM;
}
double stepper_get_position_z() {
  return stepper_position[Z_AXIS]/CONFIG_Z_STEPS_PER_MM;
}
void stepper_set_position(double x, double y, double z) {
  stepper_synchronize();  // wait until processing is done
  stepper_position[X_AXIS] = floor(x*CONFIG_X_STEPS_PER_MM + 0.5);
  stepper_position[Y_AXIS] = floor(y*CONFIG_Y_STEPS_PER_MM + 0.5);
  stepper_position[Z_AXIS] = floor(z*CONFIG_Z_STEPS_PER_MM + 0.5);  
}




// The Stepper Reset ISR
// It resets the motor port after a short period completing one step cycle.
// TODO: It is possible for the serial interrupts to delay this interrupt by a few microseconds, if
// they execute right before this interrupt. Not a big deal, but could use some TLC at some point.
ISR(TIMER2_OVF_vect) {
  // reset step pins
  STEPPING_PORT = (STEPPING_PORT & ~STEPPING_MASK) | (INVERT_MASK & STEPPING_MASK);
  TCCR2B = 0; // Disable Timer2 to prevent re-entering this interrupt when it's not needed. 
}
  

// The Stepper ISR
// This is the workhorse of LasaurGrbl. It is executed at the rate set with
// config_step_timer. It pops blocks from the block_buffer and executes them by pulsing the stepper pins appropriately.
// The bresenham line tracer algorithm controls all three stepper outputs simultaneously.
ISR(TIMER1_COMPA_vect) {
  if (busy) { return; } // The busy-flag is used to avoid reentering this interrupt
  busy = true;
  if (stop_requested) {
    // go idle and absorb any blocks
    stepper_go_idle(); 
    planner_reset_block_buffer();
    planner_request_position_update();
    gcode_request_position_update();
    busy = false;
    return;
  }
  
  if (SENSE_ANY) {
    // stop/pause program
    if (SENSE_LIMITS) {
      stepper_request_stop(STATUS_STOP_LIMIT_HIT);
    } else if (SENSE_CHILLER_OFF) {
      stepper_request_stop(STATUS_STOP_CHILLER_OFF);
    } else if (SENSE_POWER_OFF) {
      stepper_request_stop(STATUS_STOP_POWER_OFF);
    } else if(SENSE_DOOR_OPEN) {
      // no stop request
      // simply suspend processing
    }
    busy = false;
    return;    
  }

  
  // pulse steppers
  STEPPING_PORT = (STEPPING_PORT & ~DIRECTION_MASK) | (out_bits & DIRECTION_MASK);
  STEPPING_PORT = (STEPPING_PORT & ~STEPPING_MASK) | out_bits;
  // prime for reset pulse in CONFIG_PULSE_MICROSECONDS
  TCNT2 = -(((CONFIG_PULSE_MICROSECONDS-2)*CYCLES_PER_MICROSECOND) >> 3); // Reload timer counter
  TCCR2B = (1<<CS21); // Begin timer2. Full speed, 1/8 prescaler

  // Re-enable interrupts to allow ISR_TIMER2_OVERFLOW to trigger on-time and allow serial communications
  // regardless of time in this handler. The following code prepares the stepper driver for the next
  // step interrupt compare and will always finish before returning to the main program.
  sei();

  // If there is no current block, attempt to pop one from the buffer
  if (current_block == NULL) {
    // Anything in the buffer?
    current_block = planner_get_current_block();
    // if still no block command, go idle, disable interrupt
    if (current_block == NULL) {
      stepper_go_idle();
      busy = false;
      return;       
    }      
    if (current_block->type == TYPE_LINE) {  // starting on new line block
      adjusted_rate = current_block->initial_rate;
      acceleration_tick_counter = CYCLES_PER_ACCELERATION_TICK/2; // start halfway, midpoint rule.
      adjust_speed( adjusted_rate ); // initialize cycles_per_step_event
      counter_x = -(current_block->step_event_count >> 1);
      counter_y = counter_x;
      counter_z = counter_x;
      step_events_completed = 0;
    }
  }

  // process current block, populate out_bits (or handle other commands)
  switch (current_block->type) {
    case TYPE_LINE:
      ////// Execute step displacement profile by bresenham line algorithm
      out_bits = current_block->direction_bits;
      counter_x += current_block->steps_x;
      if (counter_x > 0) {
        out_bits |= (1<<X_STEP_BIT);
        counter_x -= current_block->step_event_count;
        // also keep track of absolute position
        if ((out_bits >> X_DIRECTION_BIT) & 1 ) {
          stepper_position[X_AXIS] -= 1;
        } else {
          stepper_position[X_AXIS] += 1;
        }        
      }
      counter_y += current_block->steps_y;
      if (counter_y > 0) {
        out_bits |= (1<<Y_STEP_BIT);
        counter_y -= current_block->step_event_count;
        // also keep track of absolute position
        if ((out_bits >> Y_DIRECTION_BIT) & 1 ) {
          stepper_position[Y_AXIS] -= 1;
        } else {
          stepper_position[Y_AXIS] += 1;
        }        
      }
      counter_z += current_block->steps_z;
      if (counter_z > 0) {
        out_bits |= (1<<Z_STEP_BIT);
        counter_z -= current_block->step_event_count;
        // also keep track of absolute position        
        if ((out_bits >> Z_DIRECTION_BIT) & 1 ) {
          stepper_position[Z_AXIS] -= 1;
        } else {
          stepper_position[Z_AXIS] += 1;
        }        
      }
      //////
      
      step_events_completed++;  // increment step count
      
      // apply stepper invert mask
      out_bits ^= INVERT_MASK;

      ////////// SPEED ADJUSTMENT
      if (step_events_completed < current_block->step_event_count) {  // block not finished
      
        // accelerating
        if (step_events_completed < current_block->accelerate_until) {
          if ( acceleration_tick() ) {  // scheduled speed change
            adjusted_rate += current_block->rate_delta;
            if (adjusted_rate > current_block->nominal_rate) {  // overshot
              adjusted_rate = current_block->nominal_rate;
            }
            adjust_speed( adjusted_rate );
          }
        
        // deceleration start
        } else if (step_events_completed == current_block->decelerate_after) {
            // reset counter, midpoint rule
            // makes sure deceleration is performed the same every time
            acceleration_tick_counter = CYCLES_PER_ACCELERATION_TICK/2;
                 
        // decelerating
        } else if (step_events_completed >= current_block->decelerate_after) {
          if ( acceleration_tick() ) {  // scheduled speed change
            adjusted_rate -= current_block->rate_delta;
            if (adjusted_rate < current_block->final_rate) {  // overshot
              adjusted_rate = current_block->final_rate;
            }
            adjust_speed( adjusted_rate );
          }
        
        // cruising
        } else {
          // No accelerations. Make sure we cruise exactly at the nominal rate.
          if (adjusted_rate != current_block->nominal_rate) {
            adjusted_rate = current_block->nominal_rate;
            adjust_speed( adjusted_rate );
          }
        }
      } else {  // block finished
        current_block = NULL;
        planner_discard_current_block();
      }
      ////////// END OF SPEED ADJUSTMENT
    
      break; 

    case TYPE_AIRGAS_DISABLE:
      control_air(false);
      control_gas(false);
      current_block = NULL;
      planner_discard_current_block();  
      break;

    case TYPE_AIR_ENABLE:
      control_air(true);
      current_block = NULL;
      planner_discard_current_block();  
      break;

    case TYPE_GAS_ENABLE:
      control_gas(true);
      current_block = NULL;
      planner_discard_current_block();  
      break;      
  }
  
  busy = false;
}




// This function determines an acceleration velocity change every CYCLES_PER_ACCELERATION_TICK by
// keeping track of the number of elapsed cycles during a de/ac-celeration. The code assumes that
// step_events occur significantly more often than the acceleration velocity iterations.
static bool acceleration_tick() {
  acceleration_tick_counter += cycles_per_step_event;
  if(acceleration_tick_counter > CYCLES_PER_ACCELERATION_TICK) {
    acceleration_tick_counter -= CYCLES_PER_ACCELERATION_TICK;
    return true;
  } else {
    return false;
  }
}


// Configures the prescaler and ceiling of timer 1 to produce the given rate as accurately as possible.
// Returns the actual number of cycles per interrupt
static uint32_t config_step_timer(uint32_t cycles) {
  uint16_t ceiling;
  uint16_t prescaler;
  uint32_t actual_cycles;
  if (cycles <= 0xffffL) {
    ceiling = cycles;
    prescaler = 0; // prescaler: 0
    actual_cycles = ceiling;
  } else if (cycles <= 0x7ffffL) {
    ceiling = cycles >> 3;
    prescaler = 1; // prescaler: 8
    actual_cycles = ceiling * 8L;
  } else if (cycles <= 0x3fffffL) {
    ceiling = cycles >> 6;
    prescaler = 2; // prescaler: 64
    actual_cycles = ceiling * 64L;
  } else if (cycles <= 0xffffffL) {
    ceiling = (cycles >> 8);
    prescaler = 3; // prescaler: 256
    actual_cycles = ceiling * 256L;
  } else if (cycles <= 0x3ffffffL) {
    ceiling = (cycles >> 10);
    prescaler = 4; // prescaler: 1024
    actual_cycles = ceiling * 1024L;
  } else {
    // Okay, that was slower than we actually go. Just set the slowest speed
    ceiling = 0xffff;
    prescaler = 4;
    actual_cycles = 0xffff * 1024;
  }
  // Set prescaler
  TCCR1B = (TCCR1B & ~(0x07<<CS10)) | ((prescaler+1)<<CS10);
  // Set ceiling
  OCR1A = ceiling;
  return(actual_cycles);
}


static void adjust_speed( uint32_t steps_per_minute ) {
  if (steps_per_minute < MINIMUM_STEPS_PER_MINUTE) { steps_per_minute = MINIMUM_STEPS_PER_MINUTE; }
  cycles_per_step_event = config_step_timer((CYCLES_PER_MICROSECOND*1000000*60)/steps_per_minute);

  // run at constant intensity for now
  control_laser_intensity(current_block->nominal_laser_intensity);
}





static void homing_cycle(bool x_axis, bool y_axis, bool z_axis, bool reverse_direction, uint32_t microseconds_per_pulse) {
  
  uint32_t step_delay = microseconds_per_pulse - CONFIG_PULSE_MICROSECONDS;
  uint8_t out_bits = DIRECTION_MASK;
  uint8_t limit_bits;
  uint8_t x_overshoot_count = 6;
  uint8_t y_overshoot_count = 6;
  
  if (x_axis) { out_bits |= (1<<X_STEP_BIT); }
  if (y_axis) { out_bits |= (1<<Y_STEP_BIT); }
  if (z_axis) { out_bits |= (1<<Z_STEP_BIT); }
  
  // Invert direction bits if this is a reverse homing_cycle
  if (reverse_direction) {
    out_bits ^= DIRECTION_MASK;
  }
  
  // Apply the global invert mask
  out_bits ^= INVERT_MASK;
  
  // Set direction pins
  STEPPING_PORT = (STEPPING_PORT & ~DIRECTION_MASK) | (out_bits & DIRECTION_MASK);
  
  for(;;) {
    limit_bits = LIMIT_PIN;
    if (reverse_direction) {         
      // Invert limit_bits if this is a reverse homing_cycle
      limit_bits ^= LIMIT_MASK;
    }
    if (x_axis && !(limit_bits & (1<<X1_LIMIT_BIT))) {
      if(x_overshoot_count == 0) {
        x_axis = false;
        out_bits ^= (1<<X_STEP_BIT);
      } else {
        x_overshoot_count--;
      }     
    } 
    if (y_axis && !(limit_bits & (1<<Y1_LIMIT_BIT))) {
      if(y_overshoot_count == 0) {
        y_axis = false;
        out_bits ^= (1<<Y_STEP_BIT);
      } else {
        y_overshoot_count--;
      }        
    }
    // if (z_axis && !(limit_bits & (1<<Z1_LIMIT_BIT))) {
    //   if(z_overshoot_count == 0) {
    //     z_axis = false;
    //     out_bits ^= (1<<Z_STEP_BIT);
    //   } else {
    //     z_overshoot_count--;
    //   }        
    // }
    if(x_axis || y_axis || z_axis) {
        // step all axes still in out_bits
        STEPPING_PORT |= out_bits & STEPPING_MASK;
        _delay_us(CONFIG_PULSE_MICROSECONDS);
        STEPPING_PORT ^= out_bits & STEPPING_MASK;
        _delay_us(step_delay);
    } else { 
        break;
    }
  }
  clear_vector(stepper_position);
  return;
}

static void approach_limit_switch(bool x, bool y, bool z) {
  homing_cycle(x, y, z,false, 1000);
}

static void leave_limit_switch(bool x, bool y, bool z) {
  homing_cycle(x, y, z, true, 10000);
}

void stepper_homing_cycle() {
  stepper_synchronize();  
  // home the x and y axis
  approach_limit_switch(true, true, false);
  leave_limit_switch(true, true, false);
}


