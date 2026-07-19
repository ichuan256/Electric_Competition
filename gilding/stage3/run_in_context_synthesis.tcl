# Sequential/in-context verification used when the host blocks Vivado's
# JavaScript run launcher. This does not modify project run state.
set script_dir [file dirname [file normalize [info script]]]
create_project -in_memory -part xc7z020clg400-2
add_files -norecurse [list \
    [file join $script_dir rtl thd_recon_top.v] \
    [file join $script_dir rtl thd_harmonic_reconstruction.v] \
    [file join $script_dir rtl thd_uart_v2.v] \
    [file join $script_dir rtl sine_lut_4096.mem] \
    [file join $script_dir stage3.srcs sources_1 ip clk_wiz_0 clk_wiz_0.xci]]
set_property file_type {Memory Initialization Files} \
    [get_files [file join $script_dir rtl sine_lut_4096.mem]]
set_property generate_synth_checkpoint false [get_files clk_wiz_0.xci]
read_xdc [file join $script_dir constraints thd_recon_top.xdc]
synth_design -top thd_recon_top -part xc7z020clg400-2
report_utilization -file [file join $script_dir stage3_synth_utilization.rpt]
report_timing_summary -file [file join $script_dir stage3_synth_timing.rpt]
write_checkpoint -force [file join $script_dir stage3_synth.dcp]
close_project
