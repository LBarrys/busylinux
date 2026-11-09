
/*
 * BusyLinux: 6.18's tools/sched_ext call this setter from 6.19, which 6.18
 * lacks; 6.18 lets a scheduler write the field itself.
 */
static inline void scx_bpf_task_set_dsq_vtime(struct task_struct *p, u64 vtime)
{
	p->scx.dsq_vtime = vtime;
}
