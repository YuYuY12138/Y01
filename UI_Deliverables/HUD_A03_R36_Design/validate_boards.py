"""仅验证静态画稿及元数据，不验证UE、手势、dp或业务结果。"""
from pathlib import Path
import hashlib
import json
import unittest
import zipfile
from PIL import Image

ROOT = Path(__file__).resolve().parent
META = json.loads((ROOT/'layout_manifest.json').read_text(encoding='utf-8'))
STATES = META['states']


def overlap(a, b):
    return max(0, min(a[0]+a[2], b[0]+b[2])-max(a[0],b[0])) * max(0, min(a[1]+a[3],b[1]+b[3])-max(a[1],b[1]))


class DraftChecks(unittest.TestCase):
    def test_six_states_and_phone_exports(self):
        self.assertEqual(len(STATES),6)
        for state in STATES:
            name=state['state']
            for folder,suffix,size in [('boards','',(1920,1080)),('phone_checks','_960',(960,540))]:
                with Image.open(ROOT/folder/(name+suffix+'.png')) as image:
                    self.assertEqual(image.size,size)
                    self.assertEqual(image.mode,'RGB')
                    image.verify()
        for name in ('01_combat','02_inventory'):
            with Image.open(ROOT/'phone_checks'/(name+'_20x9.png')) as image:
                self.assertEqual(image.size,(2400,1080))
        with Image.open(ROOT/'overview.png') as image:
            self.assertEqual(image.size,(1920,1840))

    def test_six_equipment_four_skill_positions_invariant(self):
        for state in STATES:
            self.assertEqual(len(state['equipment_slots']),6)
            self.assertEqual(len(state['skill_slots']),4)
            for key in ('equipment_slots','skill_slots','movement','bag_button'):
                self.assertEqual(state[key],STATES[0][key])

    def test_backpack_four_columns_stable_scroll_window(self):
        for state in STATES[1:]:
            cells=state['inventory_cells']
            self.assertEqual(len(cells),8)
            self.assertEqual(len({cell[0] for cell in cells}),4)
            self.assertEqual(len({cell[1] for cell in cells}),2)
            self.assertTrue(all(cell[2:]==[112,112] for cell in cells))
            self.assertEqual(cells,STATES[1]['inventory_cells'])
        self.assertEqual(STATES[0]['inventory_cells'],[])
        self.assertIn('不是容量上限',(ROOT/'交互说明.md').read_text(encoding='utf-8'))

    def test_all_action_rectangles_within_canvas(self):
        for state in STATES:
            rects=state['inventory_cells']+state['equipment_slots']+state['skill_slots']
            rects += [b['rect'] for b in state['buttons']]
            for rect in rects:
                x,y,w,h=rect
                self.assertGreaterEqual(x,0)
                self.assertGreaterEqual(y,0)
                self.assertLessEqual(x+w,1920)
                self.assertLessEqual(y+h,1034)

    def test_panel_occlusion_of_drop_targets_zero(self):
        for state in STATES:
            fixed=state['equipment_slots']+state['skill_slots']+[state['movement'],state['bag_button']]
            for key in ('inventory_panel','shop_panel','confirmation'):
                if state[key]:
                    for rect in fixed:
                        self.assertEqual(overlap(state[key],rect),0,(state['state'],key,rect))
            for key in ('shop_panel','confirmation'):
                if state[key]:
                    self.assertEqual(overlap(state[key],state['inventory_panel']),0)
                    self.assertEqual(overlap(state[key],[672,160,536,88]),0)
        targets=STATES[2]['equipment_slots']+STATES[2]['skill_slots']+STATES[2]['inventory_cells']
        self.assertTrue(all(overlap([728,438,562,168],r)==0 for r in targets))

    def test_buttons_and_slots_have_mobile_draft_size(self):
        for state in STATES:
            rects=[b['rect'] for b in state['buttons']]
            rects+=state['equipment_slots']+state['skill_slots']+state['inventory_cells']
            for rect in rects:
                self.assertGreaterEqual(min(rect[2:]),88)
        self.assertIn('不等于 dp',(ROOT/'交互说明.md').read_text(encoding='utf-8'))

    def test_action_rectangles_do_not_overlap(self):
        for state in STATES:
            rects=[b['rect'] for b in state['buttons']]
            rects+=state['equipment_slots']+state['skill_slots']+state['inventory_cells']
            for i,a in enumerate(rects):
                for b in rects[i+1:]:
                    self.assertEqual(overlap(a,b),0,(state['state'],a,b))

    def test_confirmation_shop_synthesis_copy(self):
        by_id={s['state']:s for s in STATES}
        confirm='\n'.join(by_id['04_skillbook_confirm']['texts'])
        self.assertIn('消耗一本技能书，旧技能不返还',confirm)
        self.assertIn('取消不耗书',confirm)
        shop='\n'.join(by_id['05_shop_delivery']['texts'])
        for value in ('随机装备','随机技能书','直接属性提升','不自动穿戴','不自动学习'):
            self.assertIn(value,shop)
        synthesis='\n'.join(by_id['06_synthesis']['texts'])
        for value in ('已选 5 / 5','目标：蓝色','退出合成','费用 / 成长待定'):
            self.assertIn(value,synthesis)

    def test_mobile_platform_and_no_forbidden_action(self):
        self.assertEqual(META['platform'],'手机横屏触控（用户已确认）')
        forbidden=['拾取','丢弃','交易','赠送','扩容','撤销','还原技能书','普攻','出售','回购']
        for state in STATES:
            for button in state['buttons']:
                self.assertTrue(all(word not in button['label'] for word in forbidden))
        self.assertFalse(META['ue_imported'])
        self.assertFalse(META['business_connected'])
        self.assertTrue(META['design_only'])

    def test_backpack_keeps_operation_sections_expanded(self):
        self.assertTrue(STATES[0]['equipment_skill_collapsible'])
        self.assertTrue(all(not s['equipment_skill_collapsible'] for s in STATES[1:]))
        for state in STATES:
            joined='\n'.join(state['texts'])
            self.assertIn('生效中',joined)
            self.assertIn('主动',joined)
            self.assertIn('被动',joined)
            self.assertIn('面板不暂停',joined)
            self.assertIn('未接 UE',joined)


def package(result):
    report={'scope':'静态坐标、文案和文件完整性，不是真机、触控命中或UE验收',
            'tests_run':result.testsRun,'failures':len(result.failures),'errors':len(result.errors),
            'panel_drop_target_overlap_px2':0,'button_slot_min_artboard_side_px':88,
            'excluded':['三角标题实际触摸热区','字体真机可读性','响应式运行布局','联网与业务逻辑']}
    (ROOT/'verification.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    paths=sorted(p for p in ROOT.rglob('*') if p.is_file() and p.suffix in ('.png','.json','.py','.md'))
    archive=ROOT/'Y01_HUD_A03_Design.zip'
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as output:
        for path in paths:
            output.write(path,path.relative_to(ROOT).as_posix())
    with zipfile.ZipFile(archive) as zipped:
        assert zipped.testzip() is None
        assert set(zipped.namelist())=={p.relative_to(ROOT).as_posix() for p in paths}
        assert all(zipped.read(p.relative_to(ROOT).as_posix())==p.read_bytes() for p in paths)
    print(f'PACKAGE_OK files={len(paths)} bytes={archive.stat().st_size}')
    print('SHA256 '+hashlib.sha256(archive.read_bytes()).hexdigest())
    print('STATIC_ONLY UE=False DEVICE=False BUSINESS=False')


if __name__=='__main__':
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(DraftChecks))
    if not result.wasSuccessful():
        raise SystemExit(1)
    package(result)
