from __future__ import print_function
import os, sys, unittest
sys.path.insert(0,os.path.abspath(os.path.join(os.path.dirname(__file__),'../../tools/sse')))
import sse_receive_ops as ops
class PolicyTest(unittest.TestCase):
    def policy(self):
        return ops.resource_policy({'receive_cpu':8,'dispatch_cpu':16,'journal_cpu':24,'prediction_cpu':136}, {'td_cpu':40,'model_version':'v0.6'}, {'SSE_PREDICTION_CPUS':'144,152,248','SSE_AUDIT_CPU':'48','SSE_OMS_JOURNAL_CPU':'56','SSE_ATP_TRACE_CPU':'64','SSE_STRATEGY_CPU':'132','SSE_SHM_READER_CPU':'129'},set(range(256)))
    def test_every_role_protected(self):
        p=self.policy()
        self.assertTrue(set([8,16,24,40,48,49,50,51,52,56,64,128,129,132,136,144,145,152,153,248,249])<=p['protected'])
        self.assertFalse(set(p['market_irq']) & p['protected'])
        self.assertFalse(set(p['other_irq']) & (p['protected'] | set(p['market_irq'])))
    def test_conflicting_fallback_cannot_choose_prediction(self):
        p=self.policy()
        for irq in range(1024):
            target=ops.irq_target(str(irq),'other-device',set([136,144,248]),True,p)
            self.assertTrue(target)
            self.assertFalse(target & p['protected'])
    def test_market_irq_excludes_trace_and_normal_keeps_valid_cpu(self):
        p=self.policy()
        for n in range(64):self.assertTrue(ops.irq_target(str(n),ops.IFACE+'-'+str(n),set([64]),True,p)<=set(p['market_irq']))
        self.assertEqual(ops.irq_target('1','other-device',set([32]),True,p),set([32]))
        self.assertEqual(ops.irq_target('1','other-device',set([32,136]),True,p),set([32]))
    def test_configuration_change_updates_policy(self):
        p=ops.resource_policy({'receive_cpu':8,'dispatch_cpu':16,'journal_cpu':24,'prediction_cpu':137},{'td_cpu':40},{'SSE_PREDICTION_CPUS':'145'},set(range(256)))
        self.assertTrue(set([137,145])<=p['protected']);self.assertNotIn(136,p['protected']);self.assertEqual(p['roles']['model_workers'],set())
    def test_unproven_irq_conflict_is_not_silently_allowed(self):
        original_irqs,original_read=ops.irqs,ops.read
        try:
            ops.irqs=lambda:[('999999','other-device',[0]*256)]
            def fake_read(path):
                if path=='/proc/interrupts':return '999999: 0 PCI-MSI other-device'
                if path.endswith('/smp_affinity_list'):return '136'
                if path.endswith('/boot_id'):return 'test-boot-never-matches'
                return original_read(path)
            ops.read=fake_read
            self.assertRaises(ValueError,ops.check_irq_affinity,self.policy())
        finally:ops.irqs,ops.read=original_irqs,original_read
if __name__=='__main__':unittest.main()
