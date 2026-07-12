# 在已打开 project_1.xpr 的 Vivado Tcl Console 中执行：
# source restore_main_design.tcl

set script_dir [file dirname [file normalize [info script]]]
set min_xdc [file join $script_dir project_1.srcs constrs_1 new ad9744_minimal_test.xdc]

# 完整功能顶层与最小测试层共用同一个150 MHz时钟IP配置。
source [file join $script_dir create_clock_ip.tcl]

set_property top ad9744_dds_top [get_filesets sources_1]

foreach old_xdc [get_files -quiet {ad9744_lcd_adapter.xdc ad9744_ch2.xdc}] {
    set_property used_in_synthesis true $old_xdc
    set_property used_in_implementation true $old_xdc
}
set min_xdc_obj [get_files -quiet $min_xdc]
if {[llength $min_xdc_obj] != 0} {
    set_property used_in_synthesis false $min_xdc_obj
    set_property used_in_implementation false $min_xdc_obj
}

update_compile_order -fileset sources_1
reset_run synth_1
puts "已恢复 ad9744_dds_top；请重新运行综合、实现并生成 bitstream。"
