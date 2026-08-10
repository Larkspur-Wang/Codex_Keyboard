# Codex Task Radio - Design Spec

> Human-readable design narrative. Machine-readable values are locked in `spec_lock.md`.

## I. Project Information

| Item | Value |
| --- | --- |
| **Project Name** | Codex 任务电台：一把键盘，远程开发 |
| **Canvas Format** | PPT 16:9 (1280 × 720) |
| **Page Count** | 36 pages (30 main + 6 appendix) |
| **Design Style** | General versatile; 通俗教学 + 桌面任务电台 + 轻工业硬件实验室 |
| **Target Audience** | 对 AI 编程和硬件有兴趣，但不一定懂 ESP32、Rust 或网络协议的学员与产品人 |
| **Use Case** | 60 分钟公开课程，含 8-10 分钟真机演示 |
| **Created Date** | 2026-08-10 |

## II. Canvas Specification

| Property | Value |
| --- | --- |
| **Format** | PPT 16:9 |
| **Dimensions** | 1280 × 720 px |
| **viewBox** | `0 0 1280 720` |
| **Margins** | left/right 64px; top 48px; bottom 42px |
| **Content Area** | 1152 × 630 px |

## III. Visual Theme

### Theme Style

- **Style**: restrained product teaching deck with physical hardware evidence
- **Theme**: light theme, occasional graphite full-bleed anchor pages
- **Tone**: practical, candid, warm, technical without showing off
- **Visual rule**: real hardware and real App screenshots prove the case; native SVG diagrams explain the system; AI illustrations only carry conceptual scenes and never impersonate a product photo.

### Color Scheme

| Role | HEX | Purpose |
| --- | --- | --- |
| **Background** | `#F3F5F3` | Fog-white teaching field |
| **Secondary bg** | `#FFFFFF` | Screenshot and evidence panels |
| **Primary** | `#1E9668` | Task activity, completed state, key arrows |
| **Accent** | `#2D7D9A` | Voice and channel paths |
| **Secondary accent** | `#E0A636` | Unread mailbox and attention |
| **Activity orange** | `#D9822B` | Two bound tasks running |
| **Activity purple** | `#7A4DB3` | Three bound tasks running |
| **Fault accent** | `#D65C4A` | Failures and hard lessons |
| **Body text** | `#19211F` | Main text and dark anchor pages |
| **Secondary text** | `#5D6864` | Captions and supporting copy |
| **Border/divider** | `#CBD2CE` | Quiet structure |

### AI Image Strategy

- **Image Rendering**: `vector-illustration`
- **Image Palette**: `cool-corporate`
- **Content rule**: use the exact deck colors as flat zones; keep fog white dominant, graphite as outline, green and blue as functional accents, yellow/coral only when semantically needed; no gradients, no written words, no fake UI, no fake hardware branding.

## IV. Typography System

### Font Plan

**Typography direction**: CJK-first modern sans; heavy titles, calm body text, no decorative “AI” typography.

| Role | Chinese | English | Fallback tail |
| --- | --- | --- | --- |
| **Title** | `Microsoft YaHei`, `PingFang SC` | `Arial Black` | sans-serif |
| **Body** | `Microsoft YaHei`, `PingFang SC` | `Arial` | sans-serif |
| **Emphasis** | `Microsoft YaHei`, `PingFang SC` | `Arial` | sans-serif |
| **Code** | — | `Consolas`, `Courier New` | monospace |

- Title: `"Arial Black", "Arial Unicode MS", "Microsoft YaHei", Arial, sans-serif`
- Body: `Arial, "Arial Unicode MS", "Microsoft YaHei", sans-serif`
- Emphasis: same as Body
- Code: `Consolas, "Courier New", monospace`

### Font Size Hierarchy

- Body baseline: 22px
- Cover title: 76px
- Chapter/anchor title: 50px
- Page title: 38px
- Subtitle: 28px
- Annotation: 15px
- Footer/page number: 12px

## V. Layout Principles

- Anchor pages use one strong statement, large negative space, and one image or diagram.
- Dense pages use dividers, process rails, image-plus-evidence layouts, and at most three parallel blocks.
- Breathing pages avoid card grids. They let one sentence, one gesture, or one real image land.
- Do not put cards inside cards. Cards use 8px radius at most.
- Use a thin top signal line and small page number as the common system, except on full-bleed anchor pages.
- Safe margin 64px; common block gap 28px; icon-text gap 12px; card padding 24px.

## VI. Icon Usage Specification

- **Primary library**: `phosphor-duotone`
- **Brand exception**: `simple-icons` for OpenAI and GitHub only.
- Approved icons: `microphone`, `speaker-high`, `wifi-high`, `broadcast`, `radio`, `lightbulb`, `keyboard`, `desktop`, `code`, `brackets-angle`, `check-circle`, `warning-circle`, `bug`, `arrow-right`, `timer`, `database`, `lock-key`, `shield-check`, `cloud-arrow-up`.
- Icons are explanatory markers, not decoration. Hardware photos remain the primary evidence.

## VII. Visualization Plan

All architecture and sequence pages are native SVG, not AI images. The main recurring motifs are:

1. Four radio channels: four aligned tracks labeled 1-4.
2. Input/output split: blue for voice into the system, green for completed work, yellow for unread audio.
3. Three-part architecture: keyboard (body), Mac Host (housekeeper), Codex (executor).
4. Evidence ladder: static checks → automated tests → runtime packets → physical audible result.

No numerical data chart is required, so no chart template is locked.

## VIII. Image Resource List

| Filename | Dimensions | Purpose | Placement Pattern | Source | Status | Text Policy | Page Role |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `cover-task-radio.png` | 1280×720 | Cover desktop task-radio scene | #1 Full-bleed background + #29 two-stop scrim | ai | Pending | none | hero_page |
| `multitask-chaos.png` | 800×560 | Four Codex jobs competing for attention | #3 Right-third image + left text | ai | Pending | none | local, scene |
| `idea-eight-key-board.png` | 1280×720 | Idea moment around an eight-key board | #1 Full-bleed background + #29 two-stop scrim | ai | Pending | none | hero_page |
| `future-ai-hardware.png` | 1280×720 | Future extensions around the same board | #1 Full-bleed background + #29 two-stop scrim | ai | Pending | none | hero_page |
| `easyinput-v2-assembled-front.png` | 1600×900 | Real assembled keyboard | image + native callouts | existing | Existing | none | local |
| `easyinput-default-keymap.png` | 1600×900 | Original keymap and board concept | right image + left statement | existing | Existing | none | local |
| `easyinput-v2-pcb-front.png` | 3840×2160 | Real PCB front | two-image evidence strip | existing | Existing | none | local |
| `easyinput-v2-pcb-back.png` | 3840×2160 | Real PCB back | two-image evidence strip | existing | Existing | none | local |
| `easyinput-v2-schematic-1.png` | 3840×2160 | Public schematic evidence | appendix no-crop | existing | Existing | none | local |
| `easyinput-v2-schematic-2.png` | 3840×2160 | Public schematic evidence | appendix no-crop | existing | Existing | none | local |
| `codex-keyboard-app-dashboard-sanitized.png` | 1640×1800 | Sanitized real App UI | #3 Right image + left workflow | existing | Existing | none | local |
| `codex-keyboard-app-icon.png` | 1254×1254 | App identity | closing/repository mark | existing | Existing | none | local |

## IX. Content Outline

| Page | Rhythm | Title | Communication Goal |
| --- | --- | --- | --- |
| P01 | anchor | Codex 任务电台 | Establish the memorable case name and physical premise |
| P02 | dense | Lark：从嵌入式软件到 AI 产品 | Give only the background needed to trust the story |
| P03 | breathing | 20 秒看懂它 | Preview speak, work, light, listen |
| P04 | anchor | 把四个 Codex 任务，变成四个实体频道 | Define the product in one sentence |
| P05 | dense | 我每天同时开着好几个 Codex 任务 | Show the real work setting |
| P06 | dense | 麻烦不是 AI 不够快，是我一直在打断自己 | Name typing, switching, waiting |
| P07 | breathing | AI 能并行，人却只有一双手 | Land the scheduling bottleneck |
| P08 | anchor | 手边正好有一块八键开发板 | Explain the idea moment |
| P09 | anchor | 四个任务，就是四个频道 | Make the walkie-talkie metaphor memorable |
| P10 | dense | 最初的交互草图 | Map top keys, bottom keys, four lights |
| P11 | breathing | EasyInput 的本质是一块开发板 | Reframe the keyboard |
| P12 | dense | 用人话认识这块硬件 | Press, turn, hear, speak, light |
| P13 | breathing | 这次真正用上了四个原本闲着的能力 | PSRAM, Wi-Fi audio, amplifier, speaker |
| P14 | breathing | 第一版先把本地 Wi-Fi 跑通 | Set an honest boundary for “remote” |
| P15 | dense | Mac App 负责绑定四个任务 | Show the four-slot dashboard |
| P16 | dense | 按住说，松开就发 | Explain S1-S4 input |
| P17 | breathing | 发完就走，Codex 在后台干活 | Show the human benefit |
| P18 | dense | 哪个任务有回信，哪颗灯就亮 | Explain mailbox brightness |
| P19 | dense | S5-S8：按一下，听对应任务汇报 | Explain playback mapping |
| P20 | anchor | 键盘是身体，Mac 是管家，Codex 是执行者 | Explain the architecture without jargon |
| P21 | dense | 说进去：麦克风 → 千问 ASR → Codex | Input path |
| P22 | dense | 做完后：Codex → Spark 总结 | Completion and summarization |
| P23 | dense | 听回来：千问 TTS → 本地缓存 → 扬声器 | Output path |
| P24 | breathing | 真正播完，才算“听过” | Explain exact heard semantics |
| P25 | dense | 从现有键盘，抽出自己的 App 和固件 | Explain repository extraction and ownership |
| P26 | dense | 第一次卡住：进不了下载模式，有开机音却没播放 | Keep the failure story real |
| P27 | dense | 更难的是那些“看起来差不多”的问题 | Transfer, LED direction, stale TTS state |
| P28 | breathing | 测试通过，不等于耳朵真的听到了 | Separate evidence layers |
| P29 | breathing | 学员还能把它改成什么 | Invite extension ideas |
| P30 | anchor | AI 硬件，是让 AI 进入日常工作 | Close on a concrete idea, not hype |
| P31 | dense | 附录：EasyInput V2 硬件参数 | Public hardware facts |
| P32 | dense | 附录：S1-S8 与 GPIO 对照 | Exact physical mapping |
| P33 | dense | 附录：语音输入时序 | Technical input sequence |
| P34 | dense | 附录：总结、缓存与播放时序 | Technical output sequence |
| P35 | dense | 附录：密钥、缓存和数据放在哪里 | Local security and ownership |
| P36 | dense | 附录：仓库与测试入口 | Help learners continue |

## X. Writing Rules

- Speak like a person teaching a real case. Use “我做了什么、为什么这么做、哪里出过问题”.
- Prefer concrete verbs: 按住、说、绑定、生成、缓存、播放、删除、重试.
- Avoid: 赋能、重塑、范式、生态、闭环、全链路、端到端.
- No internal monologue, no fake dialogue, no made-up user quote.
- On-slide text stays short. Speaker notes carry context and transitions.
- Current implementation is same-LAN. Never describe it as finished public-Internet control.

## XI. Delivery

- 36 editable SVG source pages
- 36-page PPTX with speaker notes
- PDF preview
- Contact sheet and page PNG renders for visual checking
- All real screenshots sanitized; no API key, Wi-Fi password, local path, or Codex task ID appears on a slide
