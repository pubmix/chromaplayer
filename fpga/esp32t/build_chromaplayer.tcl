open_project evt1_x2.gprj
set_option -use_sspi_as_gpio 1
set_option -power_on_reset_monitor 1
set_option -use_i2c_as_gpio 1
set_option -use_cpu_as_gpio 1
set_option -multi_boot 0
set_option -bit_format bin
set_option -bit_security 0
set_option -user_code 43505231
set_option -bg_programming jtag_sspi_qsspi
set_option -use_mspi_as_gpio 1
set_option -opt_goal area
set_option -place_option 2
set_option -route_option 2
set_option -gen_text_timing_rpt 1
run all
exit


