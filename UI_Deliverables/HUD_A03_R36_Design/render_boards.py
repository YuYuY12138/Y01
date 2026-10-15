"""A03 离线状态稿制图工具；不运行游戏、不导入 UE、不实现任何业务或交互。"""
from pathlib import Path
from functools import lru_cache
import json
import math
import hashlib
import zipfile
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
OLD = ROOT.parent / "HUD_A02_R27_Assets_v001"
FONT = "C:/Windows/Fonts/msyh.ttc"
BOLD = "C:/Windows/Fonts/msyhbd.ttc"
W, H = 1920, 1080
C = dict(bg="#101923", panel="#182633", line="#506779", text="#EDF3F5",
         muted="#A9BDC8", cyan="#55DCCF", green="#72D495", blue="#63AEF1",
         purple="#B995ED", gold="#E9C773", red="#EF7B79", dark="#0D1820")
STATES = [
    ("01_combat", "01 / 常态战斗", "背包收起 · 操作位置不变"),
    ("02_inventory", "02 / 背包展开", "装备、技能、背包同时可见"),
    ("03_equipment_drag", "03 / 装备拖动替换", "合法目标高亮 · 旧装备回包"),
    ("04_skillbook_confirm", "04 / 技能书覆盖", "确认消耗 · 旧技能不返还"),
    ("05_shop_delivery", "05 / 商店购买入包", "商店与背包并排 · 结果回执示意"),
    ("06_synthesis", "06 / 背包五合一", "同类型同品质五份 · 随机高一级"),
]
EQUIP = (472, 808, 762, 210)
BAG_BUTTON = (1254, 890, 216, 100)
BAG = (1360, 192, 532, 482)
SKILLS = (1512, 694, 380, 324)
MOVE = (28, 756, 296, 262)
SHOP = (874, 260, 456, 500)
CONFIRM = (752, 346, 578, 390)
LAYOUTS = []
TEXTS = []
BUTTONS = []


@lru_cache(None)
def font(size, bold=False):
    return ImageFont.truetype(BOLD if bold else FONT, size)


def rr(im, box, fill=None, outline=None, radius=10, width=1):
    x, y, w, h = box
    ImageDraw.Draw(im).rounded_rectangle((x, y, x + w - 1, y + h - 1), radius,
                                         fill=fill, outline=outline, width=width)


def txt(im, x, y, value, size=20, color=None, bold=False):
    d = ImageDraw.Draw(im)
    box = d.textbbox((x, y), value, font=font(size, bold))
    assert box[0] >= 0 and box[1] >= 0 and box[2] <= W and box[3] <= H, (value, box)
    d.text((x, y), value, font=font(size, bold), fill=color or C['text'])
    TEXTS.append(value)


def line(im, points, color=None, width=2):
    ImageDraw.Draw(im).line(points, fill=color or C['line'], width=width, joint="curve")


def arrow(im, points, color=None, dashed=False, width=3):
    color = color or C['cyan']
    d = ImageDraw.Draw(im)
    for a, b in zip(points, points[1:]):
        length = math.dist(a, b)
        if dashed:
            for s in range(0, math.ceil(length), 18):
                start = tuple(a[k] + (b[k] - a[k]) * s / length for k in (0, 1))
                end = tuple(a[k] + (b[k] - a[k]) * min(s + 10, length) / length for k in (0, 1))
                d.line((start, end), fill=color, width=width)
        else:
            d.line((a, b), fill=color, width=width)
    a, b = points[-2:]
    angle = math.atan2(b[1] - a[1], b[0] - a[0])
    triangle = [b] + [(b[0] - 14 * math.cos(angle + sign * .45),
                      b[1] - 14 * math.sin(angle + sign * .45)) for sign in (-1, 1)]
    d.polygon(triangle, fill=color)


@lru_cache(None)
def original(name):
    with Image.open(OLD / 'png' / f'{name}.png') as source:
        return source.convert('RGBA')


def icon(im, name, x, y, size=32, color=None):
    source = original(name).resize((size, size), Image.Resampling.LANCZOS)
    if color:
        layer = Image.new('RGBA', source.size, color)
        layer.putalpha(source.getchannel('A'))
        source = layer
    im.alpha_composite(source, (x, y))


def frame(im, name, x, y, width, height, margin=18):
    source = original(name)
    sw, sh = source.size
    sx, sy = [0, margin, sw-margin, sw], [0, margin, sh-margin, sh]
    dx, dy = [0, margin, width-margin, width], [0, margin, height-margin, height]
    for r in range(3):
        for c in range(3):
            crop = source.crop((sx[c], sy[r], sx[c+1], sy[r+1]))
            crop = crop.resize((dx[c+1]-dx[c], dy[r+1]-dy[r]), Image.Resampling.LANCZOS)
            im.alpha_composite(crop, (x+dx[c], y+dy[r]))


def section(im, box, title, subtitle=None, collapse=True):
    x, y, w, h = box
    rr(im, box, C['panel'], C['line'], 12)
    txt(im, x+18, y+13, title, 28, bold=True)
    if subtitle:
        txt(im, x+18, y+54, subtitle, 24, C['muted'])
    if collapse:
        icon(im, 'chevron_down', x+w-54, y+22, 32, C['cyan'])


def button(im, box, label, state='normal', size=20, color=None):
    x, y, w, h = box
    frame(im, 'button_'+state, x, y, w, h, 14)
    length = ImageDraw.Draw(im).textlength(label, font=font(size, True))
    assert length <= w-12, (label, length, w)
    txt(im, x+(w-length)//2, y+(h-size)//2-3, label, size, color, True)
    BUTTONS.append({'label':label,'rect':list(box)})


def pill(im, x, y, label, color=None, size=16):
    color = color or C['cyan']
    width = math.ceil(ImageDraw.Draw(im).textlength(label, font=font(size)))+20
    rr(im, (x, y, width, size+12), '#203744', color, 6)
    txt(im, x+10, y+3, label, size, color)


def health(im, x, y, w, fraction, color):
    rr(im, (x, y, w, 13), C['dark'], '#658290', 6)
    rr(im, (x+3, y+3, max(2, round((w-6)*fraction)), 7), color, radius=3)


def item_icon(im, kind, x, y, size, color):
    d = ImageDraw.Draw(im)
    def p(a, b):
        return (x+round(a*size), y+round(b*size))
    if kind == 'book':
        d.polygon([p(.2,.14), p(.7,.08), p(.84,.22), p(.84,.84), p(.3,.94), p(.2,.84)],
                  fill='#26364C', outline=color, width=3)
        line(im, [p(.32,.22), p(.32,.86)], color, 2)
        line(im, [p(.35,.26), p(.73,.2)], color, 2)
        line(im, [p(.45,.48), p(.7,.43)], color, 3)
        line(im, [p(.48,.59), p(.66,.55)], color, 2)
    elif kind == 'armor':
        d.polygon([p(.28,.14), p(.42,.25), p(.58,.25), p(.72,.14), p(.96,.36),
                   p(.78,.56), p(.68,.46), p(.7,.9), p(.3,.9), p(.32,.46), p(.22,.56), p(.04,.36)],
                  fill='#263A44', outline=color, width=3)
        line(im, [p(.5,.3), p(.5,.78)], color, 2)
    elif kind == 'shield':
        d.polygon([p(.18,.16), p(.5,.07), p(.82,.16), p(.77,.65), p(.5,.92), p(.23,.65)],
                  fill='#263A44', outline=color, width=3)
        line(im, [p(.5,.2), p(.5,.73)], color, 3)
    else:
        d.polygon([p(.79,.08), p(.85,.25), p(.41,.71), p(.3,.6)], fill=color)
        line(im, [p(.18,.58), p(.46,.85)], color, 4)
        line(im, [p(.32,.73), p(.12,.94)], color, 6)


def item(im, x, y, kind, quality, label, size=88, selected=False, empty=False, dim=False):
    rr(im, (x, y, size, size), C['dark'], radius=10)
    if empty:
        icon(im, 'slot_empty', x, y, size)
        return
    frame(im, 'quality_'+quality, x, y, size, size)
    item_icon(im, kind, x+size//2-27, y+8, 54, C[quality])
    txt(im, x+9, y+size-33, label, 22, C[quality])
    if selected:
        rr(im, (x-1,y-1,size+2,size+2), outline=C['cyan'], width=3)
        rr(im, (x+size-27,y+4,23,22), C['cyan'], radius=4)
        line(im, [(x+size-23,y+14),(x+size-18,y+19),(x+size-9,y+9)],C['dark'],3)
    if dim:
        im.alpha_composite(Image.new('RGBA', (size,size), (10,20,30,155)), (x,y))


def scene(im, state):
    d = ImageDraw.Draw(im)
    for x in range(-500, W, 100):
        line(im, [(x,1022),(x+740,150)], '#192833', 1)
    for y in range(150, 1022, 84):
        line(im, [(330,y),(1340,y)], '#192833', 1)
    for x,y,r in [(792,584,175),(1010,450,74)]:
        d.ellipse((x-r,y-r//2,x+r,y+r//2), outline='#29414D', width=2)
    for x,y,label,color,fraction in [(800,552,'你',C['green'],.82),
                                      (650,426,'战利品仓',C['cyan'],.75),
                                      (1098,450,'小怪',C['red'],.5),
                                      (514,633,'队友',C['blue'],.78)]:
        txt(im,x-38,y-54,label,18,C['muted'])
        health(im,x-48,y-24,110,fraction,color)
        d.ellipse((x-20,y+10,x+20,y+32), fill='#243946', outline=color, width=2)
    txt(im, 984, 529, '−20', 27, C['red'], True)
    if state == '02_inventory':
        arrow(im, [(1083,481),(984,536),(840,568)], C['purple'], True)
        for x,y in [(1060,493),(984,536),(904,555)]:
            d.ellipse((x-4,y-4,x+4,y+4), fill=C['purple'])
        pill(im, 890, 584, '① 掉落飞向角色', C['purple'], 26)
        txt(im, 722, 682, '② 到达入包 · 接收格闪亮', 28, C['muted'])


def permanent(im, state):
    section(im, (28,28,320,178), '你 · Lv.12')
    icon(im,'icon_coin',44,137,30,C['gold'])
    txt(im,80,130,'1,280',30,C['gold'],True)
    button(im,(200,115,132,88),'属性',size=26)
    section(im,(28,224,292,226),'队伍 / 伤害')
    for i,(name,pct) in enumerate([('你',36),('队友 B',29),('队友 C',21),('队友 D',14)]):
        y=302+i*35
        rr(im,(44,y+5,7,20),C['cyan'] if i==0 else C['blue'],radius=3)
        txt(im,64,y,name,24,C['cyan'] if i==0 else C['text'])
        txt(im,244,y,str(pct)+'%',24)
    section(im,(604,28,672,112),'第 05 / 15 波 · 战斗中')
    txt(im,623,89,'阶段计时',24,C['muted'])
    txt(im,1002,80,'00:38',34,C['gold'],True)
    section(im,(672,160,536,88),'BOSS · 示例首领')
    health(im,690,226,498,.60,C['red'])
    section(im,(1344,28,548,148),'经营入口')
    for i,label in enumerate(['商店','练功房','回城','发育']):
        button(im,(1356+i*132,86,126,88),label,'hover' if state=='05_shop_delivery' and i==0 else 'normal',26)
    section(im,MOVE,'移动')
    icon(im,'joystick_base',77,810,200)
    icon(im,'joystick_knob',137,870,80)
    section(im,EQUIP,'装备 / 六槽',collapse=state=='01_combat')
    qualities=['green','blue','blue','gold','red',None]
    names=['绿·刃','蓝·甲','蓝·腕','金·盾','红·刃','空槽']
    for i in range(6):
        x,y=492+i*120,900
        item(im,x,y, ['sword','armor','armor','shield','sword','sword'][i],qualities[i],names[i],104,empty=i==5)
        if state=='03_equipment_drag':
            rr(im,(x-2,y-2,108,108),outline=C['cyan'],width=3)
            if i==2:
                pill(im,x+10,y-38,'替换',C['gold'],24)
    is_open=state!='01_combat'
    button(im,BAG_BUTTON,'收起背包' if is_open else '背包', 'hover',30,C['cyan'])
    if not is_open:
        pill(im,1396,876,'新',C['gold'],24)
    section(im,SKILLS,'技能 / 四槽',collapse=state=='01_combat')
    if is_open:
        button(im,(1360,716,126,88),'技能管理',size=26)
    for i in range(4):
        x,y=1530+(i%2)*178,786+(i//2)*116
        active=i<2
        rr(im,(x,y,158,108),C['dark'],radius=10)
        frame(im,'skill_active_frame' if active else 'skill_passive_frame',x,y,158,108)
        txt(im,x+12,y+5,f'{i+1} '+('主动' if active else '被动'),24,C['cyan'] if active else C['muted'])
        icon(im,'icon_active' if active else 'icon_passive',x+63,y+34,34,C['cyan'] if active else C['muted'])
        if i==1:
            rr(im,(x+8,y+33,142,41),'#152431',radius=4)
            txt(im,x+43,y+31,'6.2s',30,C['muted'],True)
        txt(im,x+28,y+69,['释放','冷却中','生效中','生效中'][i],24,C['text'] if active else C['muted'])
        if state=='04_skillbook_confirm' and i==1:
            rr(im,(x-2,y-2,162,112),outline=C['gold'],width=3)
    if state=='03_equipment_drag':
        pill(im,1560,681,'非装备目标',C['red'],24)


def bag(im,state):
    section(im,BAG,'背包 / 五合一' if state=='06_synthesis' else '背包',collapse=state!='06_synthesis')
    if state=='06_synthesis':
        button(im,(1690,194,176,88),'退出合成',size=26)
    txt(im,1378,246,'同类同品质材料' if state=='06_synthesis' else '装备 / 未学习技能书',24,C['muted'])
    items=[('sword','purple','装·紫'),('book','purple','书·紫'),('armor','blue','装·蓝'),
           ('book','gold','书·金'),('shield','green','装·绿'),('book','blue','书·蓝'),
           ('armor','green','装·绿'),('sword','green','装·绿'),('book','green','书·绿'),
           ('shield','purple','装·紫'),('sword','red','装·红'),('book','red','书·红'),
           ('shield','blue','装·蓝'),('book','purple','书·紫'),('armor','gold','装·金')]
    if state=='05_shop_delivery':
        items[0]=('armor','blue','装·蓝')
    if state=='06_synthesis':
        for i,kind in enumerate(['sword','armor','shield','sword','armor']):
            items[i]=(kind,'green','装·绿')
    cells=[]
    for i,(kind,q,label) in enumerate(items[:8]):
        x,y=1380+(i%4)*124,286+(i//4)*128
        cells.append([x,y,112,112])
        dim=state=='06_synthesis' and i>=5
        item(im,x,y,kind,q,label,size=112,selected=state=='06_synthesis' and i<5,dim=dim)
        if state=='03_equipment_drag' and i==0:
            rr(im,(x+6,y+6,100,100),C['dark'],C['purple'],8)
            txt(im,x+20,y+36,'拖动中',24,C['purple'])
        if state=='04_skillbook_confirm' and i==1:
            rr(im,(x-2,y-2,116,116),outline=C['gold'],width=3)
            pill(im,x+3,y-8,'待确认',C['gold'],22)
        if state in ('02_inventory','05_shop_delivery') and i==0:
            for pad in (1,5):
                rr(im,(x-pad,y-pad,112+2*pad,112+2*pad),outline=C['gold'],width=2)
            pill(im,x+65,y-8,'新',C['gold'],22)
    rr(im,(1878,286,4,240),'#344856',radius=2)
    rr(im,(1878,287,4,62),C['muted'],radius=2)
    line(im,[(1378,540),(1874,540)],C['line'],1)
    if state=='06_synthesis':
        txt(im,1378,550,'装备 · 绿    已选 5 / 5',26,C['cyan'],True)
        txt(im,1378,593,'目标：蓝色随机装备',24,C['blue'])
        txt(im,1378,632,'费用 / 成长待定',24,C['muted'])
        button(im,(1734,582,142,88),'合成', 'hover',28)
    else:
        txt(im,1378,550,'长按拖动 · 滑动滚动',26)
        txt(im,1378,593,'装备穿戴 / 卸下',24,C['muted'])
        txt(im,1378,632,'技能书拖入学习',24,C['muted'])
        button(im,(1734,582,142,88),'五合一',size=28)
    return cells


def note(im,box,title,lines,color=None):
    color=color or C['cyan']
    x,y,w,h=box
    rr(im,box,'#12232D',color,12)
    txt(im,x+18,y+12,title,22,color,True)
    for i,value in enumerate(lines):
        txt(im,x+18,y+51+i*29,value,18,C['muted'])


def special(im,state):
    if state=='01_combat':
        return
    if state=='03_equipment_drag':
        section(im,(728,438,562,168),'紫·长刃 → 装备槽 3',collapse=False)
        txt(im,748,503,'松手替换 · 旧装备回背包',28,C['gold'])
        txt(im,748,549,'非法位置松手，恢复原位',26,C['muted'])
        arrow(im,[(1402,384),(1310,405),(1310,772),(784,772),(784,894)],C['cyan'])
        item(im,1246,630,'sword','purple','装·紫',112)
        ImageDraw.Draw(im).ellipse((1280,760,1340,820),outline=C['text'],width=2)
        txt(im,1000,738,'拖影在指尖上方',26,C['muted'])
    elif state=='04_skillbook_confirm':
        section(im,CONFIRM,'覆盖技能？',collapse=False)
        x,y,_,_=CONFIRM
        txt(im,x+20,y+65,'旧：蓝·冰锥（主动，CD 中）',28,C['blue'])
        txt(im,x+20,y+112,'新：紫·回春术（主动）',28,C['purple'])
        txt(im,x+20,y+171,'消耗一本技能书，旧技能不返还',28,C['gold'],True)
        txt(im,x+20,y+221,'取消不耗书，旧技能保持不变',26,C['muted'])
        button(im,(x+20,y+286,244,88),'取消',size=28)
        button(im,(x+290,y+286,268,88),'确认学习','hover',28)
        arrow(im,[(1536,404),(1344,404)],C['gold'],True,2)
        arrow(im,[(1340,685),(1497,685),(1497,768),(1787,768),(1787,784)],C['gold'],True,2)
    elif state=='05_shop_delivery':
        section(im,SHOP,'商店',collapse=True)
        cards=[('随机装备','入包，不自动穿戴',C['blue']),
               ('随机技能书','入包，不自动学习',C['purple']),
               ('直接属性提升','直接生效，不占槽',C['green'])]
        for i,(title,desc,color) in enumerate(cards):
            y=328+i*136
            rr(im,(891,y,420,126),'#12212E','#344D5E',8)
            txt(im,905,y+8,title,28,color,True)
            txt(im,905,y+50,desc,24,C['muted'])
            txt(im,905,y+90,'档位 / 售价读配置' if i<2 else '效果 / 售价读配置',22,C['muted'])
            button(im,(1198,y+20,104,88),'购买','hover',26)
        arrow(im,[(1318,368),(1343,368),(1374,342)],C['gold'],False,2)
        section(im,(382,480,456,124),'入包反馈 · 结果状态示意',collapse=False)
        txt(im,402,546,'蓝·护腕已入包',30,C['gold'],True)
    elif state=='06_synthesis':
        section(im,(978,418,352,264),'目标：蓝色品质',collapse=False)
        txt(im,1000,485,'随机装备 ×1',32,C['blue'],True)
        txt(im,1000,537,'不预报具体条目',26,C['muted'])
        txt(im,1000,581,'结果回背包',28,C['text'])
        txt(im,1000,627,'费用 / 成长待定',24,C['muted'])


def make(state,title,desc):
    text_start, button_start = len(TEXTS), len(BUTTONS)
    im=Image.new('RGBA',(W,H),C['bg'])
    scene(im,state)
    permanent(im,state)
    cells=bag(im,state) if state!='01_combat' else []
    special(im,state)
    txt(im,360,254,'战斗继续 · 面板不暂停',24,C['muted'])
    rr(im,(0,1034,1920,46),'#0A121A',radius=0)
    txt(im,24,1043,title+'  |  '+desc,19,C['text'],True)
    txt(im,1254,1044,'A03 · 静态交互提案 · 模拟数据 · 未接 UE',18,C['gold'])
    im.convert('RGB').save(ROOT/'boards'/f'{state}.png')
    im.convert('RGB').resize((960,540),Image.Resampling.LANCZOS).save(ROOT/'phone_checks'/f'{state}_960.png')
    LAYOUTS.append(dict(state=state,size=[W,H],inventory_open=bool(cells),inventory_cells=cells,
                        texts=TEXTS[text_start:],buttons=BUTTONS[button_start:],
                        equipment_skill_collapsible=not bool(cells),
                        equipment_slots=[[492+i*120,900,104,104] for i in range(6)],
                        skill_slots=[[1530+(i%2)*178,786+(i//2)*116,158,108] for i in range(4)],
                        movement=list(MOVE),bag_button=list(BAG_BUTTON),
                        inventory_panel=list(BAG) if cells else None,
                        shop_panel=list(SHOP) if state=='05_shop_delivery' else None,
                        confirmation=list(CONFIRM) if state=='04_skillbook_confirm' else None))
    return im


def main():
    assert ROOT.name=='HUD_A03_R36_Design' and ROOT.parent.name=='UI_Deliverables'
    for folder in ('boards','phone_checks'):
        (ROOT/folder).mkdir(exist_ok=True)
    images=[make(*entry) for entry in STATES]
    for index in (0,1):
        wide=Image.new('RGB',(2400,1080),C['bg'])
        wide.paste(images[index].convert('RGB'),(240,0))
        dwide=ImageDraw.Draw(wide)
        for x in (120,2280):
            dwide.line((x,28,x,1018),fill=C['line'],width=2)
        dwide.text((16,450),'SafeZone',font=font(20),fill=C['muted'])
        dwide.text((16,483),'示意',font=font(24),fill=C['muted'])
        wide.save(ROOT/'phone_checks'/f'{STATES[index][0]}_20x9.png')
    sheet=Image.new('RGB',(1920,1840),C['bg'])
    d=ImageDraw.Draw(sheet)
    d.text((32,20),'Y01 / A03 主 HUD · 背包与技能书',font=font(36,True),fill=C['text'])
    d.text((32,74),'手机横屏触控 / 1920×1080 制图基准 / 本轮仅设计，不表示任何真实功能已接通',font=font(21),fill=C['gold'])
    for i,im in enumerate(images):
        x,y=28+(i%2)*956,126+(i//2)*568
        thumb=im.convert('RGB').resize((928,522),Image.Resampling.LANCZOS)
        sheet.paste(thumb,(x,y))
        d.text((x+6,y+528),STATES[i][1]+'  '+STATES[i][2],font=font(18),fill=C['text'])
    sheet.save(ROOT/'overview.png')
    metadata=dict(design_only=True,ue_imported=False,business_connected=False,
                  platform='手机横屏触控（用户已确认）',
                  assumptions=['1920×1080制图，960×540缩图检查；不等同于dp或真机','长按阈值与移动容差待设备验证','物品逐份展示，不预设堆叠规则'],
                  unconfirmed=['合成费用','装备材料成长继承','死亡和断线期间经营','具体机型、SafeZone及DPI适配'],
                  sources={'reused':str(OLD),'new':'离线Pillow几何绘制装备/书籍占位与布局；不复用角色或场景素材',
                           'font':'Microsoft YaHei仅本机渲染，不分发字体',
                           'licensing':'继承原素材来源记录；不额外宣称CC0或排他权，无新增采购'},
                  states=LAYOUTS,text_inventory=TEXTS)
    (ROOT/'layout_manifest.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(f'RENDERED boards={len(images)} overview=1 UE_IMPORT=False BUSINESS=False')


if __name__=='__main__':
    main()
