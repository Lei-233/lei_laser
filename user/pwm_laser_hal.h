#ifndef _PWM_LASER_HAL_H_
#define _PWM_LASER_HAL_H_

int pwm_laser_open(void);
int pwm_laser_set_target(int target);   // 0~1000
void pwm_laser_close(void);

#endif
