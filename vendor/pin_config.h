#ifndef pin_config_h
#define pin_config_h
#include <mk20dx128.h>

//: See K20 SubFamily Reference Manual
//: K20P64M72SF1RM
//: section 1.14.1 Pin Control Register



 // For resetting interrupts
#define ISF (1<<24)

// Various interrupt triggering modes

#define IRQC_MASK         0xF0000
#define IRQC_NONE         0x00000
#define IRQC_ZERO         0x80000
#define IRQC_RISING       0x90000
#define IRQC_FALLING      0xA0000
#define IRQC_EITHER_EDGE  0xB0000
#define IRQC_ONE          0xC0000

#define PULL_DOWN  2
#define PULL_UP    3
#define PULL_NONE  0

//Standard Output is :
// "GPIO"|"High Drive Strength"|"Slow Slew Rate Enabled"
#define STANDARD_OUTPUT ((1<<8) | (1<<6) | (1<<2))

#define MUX_GPIO (1<<8)

#endif
