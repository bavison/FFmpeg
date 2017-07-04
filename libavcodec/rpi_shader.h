#ifndef rpi_shader_H
#define rpi_shader_H

extern unsigned int rpi_shader[];

#define mc_setup_c_q0 (rpi_shader + 0)
#define mc_start (rpi_shader + 0)
#define mc_setup_c_qn (rpi_shader + 2)
#define mc_filter_uv (rpi_shader + 138)
#define mc_filter_uv_b0 (rpi_shader + 264)
#define mc_sync_q0 (rpi_shader + 454)
#define mc_sync_q1 (rpi_shader + 472)
#define mc_sync_q2 (rpi_shader + 484)
#define mc_sync_q3 (rpi_shader + 496)
#define mc_sync_q4 (rpi_shader + 508)
#define mc_sync_q5 (rpi_shader + 526)
#define mc_sync_q6 (rpi_shader + 538)
#define mc_sync_q7 (rpi_shader + 550)
#define mc_sync_q8 (rpi_shader + 562)
#define mc_sync_q9 (rpi_shader + 580)
#define mc_sync_q10 (rpi_shader + 592)
#define mc_sync_q11 (rpi_shader + 604)
#define mc_exit (rpi_shader + 616)
#define mc_exit_c (rpi_shader + 616)
#define mc_interrupt_exit12 (rpi_shader + 630)
#define mc_interrupt_exit12c (rpi_shader + 630)
#define mc_setup_y_q0 (rpi_shader + 646)
#define mc_setup_y_qn (rpi_shader + 648)
#define mc_filter (rpi_shader + 884)
#define mc_filter_b (rpi_shader + 1022)
#define mc_filter_y_p00 (rpi_shader + 1160)
#define mc_filter_y_b00 (rpi_shader + 1256)
#define mc_setup_c10_q0 (rpi_shader + 1340)
#define mc_setup_c10_qn (rpi_shader + 1340)
#define mc_filter_c10_p (rpi_shader + 1472)
#define mc_end (rpi_shader + 1598)

#endif
