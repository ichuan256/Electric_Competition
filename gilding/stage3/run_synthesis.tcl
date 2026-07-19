set script_dir [file dirname [file normalize [info script]]]
open_project [file join $script_dir stage3.xpr]
reset_run synth_1
launch_runs synth_1 -jobs 8
wait_on_run synth_1
if {[get_property STATUS [get_runs synth_1]] ne "synth_design Complete!"} {
    error "Stage3 synthesis failed: [get_property STATUS [get_runs synth_1]]"
}
open_run synth_1
report_utilization -file [file join $script_dir stage3_synth_utilization.rpt]
report_timing_summary -file [file join $script_dir stage3_synth_timing.rpt]
close_project
