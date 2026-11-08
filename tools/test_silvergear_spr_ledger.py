#!/usr/bin/env python3
"""Regression cases for uncertainty in the static native-SPR ledger."""
import hashlib
import struct
import unittest
from unittest.mock import patch
import silvergear_spr_ledger as ledger

class LedgerTests(unittest.TestCase):
    def test_literal_computed_jump_is_not_a_return(self):
        code={0:('l.ori',['r3','r0','0x100']),4:('l.lwz',['r6','0(r3)']),8:('l.jr',['r6']),
              16:('l.mtspr',['r0','r2','0x8868']),20:('l.jr',['r9'])}
        w=ledger.Walker(code,1,[],{0x100:16});w.walk(0,{2:ledger.C(0x182e)},0)
        self.assertEqual([(e[3],e[4].b) for e in w.events],[(0x8868,0x182e)])
        w=ledger.Walker(code,1,[]);w.walk(0,{},0)
        self.assertTrue(any('indirect jump' in u[2] for u in w.unresolved))
    def test_calls_invalidate_condition_flags(self):
        code={0:('l.sfeqi',['r3','1']),4:('l.jal',['0x40']),8:('l.bf',['0x18']),
              12:('l.mtspr',['r0','r2','0x8000']),16:('l.jr',['r9']),
              24:('l.mtspr',['r0','r2','0x8004']),28:('l.jr',['r9']),64:('l.jr',['r9'])}
        w=ledger.Walker(code,2,[]);w.walk(0,{3:ledger.C(1),2:ledger.C(7)},0)
        self.assertEqual({e[3] for e in w.events},{0x8000,0x8004})
        self.assertTrue(all(e[5] for e in w.events))

    def test_unresolved_calls_and_writes_are_visible(self):
        code={0:('l.jalr',['r4']),4:('l.mtspr',['r3','r2','0']),8:('l.jal',['0x0']),12:('l.jr',['r9'])}
        w=ledger.Walker(code,2,[]);w.walk(0,{},0)
        reasons=[u[2] for u in w.unresolved]
        self.assertTrue(any('indirect' in r for r in reasons))
        self.assertIn('unknown SPR write address',reasons)
        self.assertIn('recursive call',reasons)

    def test_signed_division_truncates_toward_zero(self):
        w=ledger.Walker({},1,[])
        self.assertEqual(ledger.signed(w.bin('div',ledger.C(-7),ledger.C(3)).b),-2)

    def test_task_table_hash_and_callback_fields(self):
        image=bytearray(4096);code={0x02008710:('l.jr',['r9']),0x02000200:('l.jr',['r9'])}
        base=ledger.APP_BASE;pc=base
        for task in range(1,11):
            descriptor=base+0x400+task*32;name=base+0x800+task*16
            struct.pack_into('<5I',image,descriptor-base,name,0,base+0x200,0,base+0x200)
            image[name-base:name-base+5]=b'Task\0'
            for op,args in [('l.addi',['r3','r0',str(task)]),('l.movhi',['r4','0x200']),
                            ('l.ori',['r4','r4',hex(descriptor&0xffff)]),('l.jal',['0x2008710'])]:
                code[pc]=(op,args);pc+=4
        code[pc]=('l.jr',['r9'])
        with self.assertRaises(ValueError):ledger.task_records(code,bytes(image))
        with patch.object(ledger,'APP_SHA256',hashlib.sha256(image).hexdigest()):
            records=ledger.task_records(code,bytes(image))
        self.assertEqual(len(records),10)
        self.assertEqual(records[0]['callbacks'],dict(init=base+0x200,exit=0,poll=base+0x200))

if __name__=='__main__':unittest.main()
