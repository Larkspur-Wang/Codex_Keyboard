# 手把手复刻教程

- [学员正文](draft.md)：本地编辑入口，2026-09-05 修订。
- [在线飞书教程](https://rx5emoyb8vg.feishu.cn/docx/R7uTdHTbMotf0px4sy6cPu7nn0f)：保留原文档、两张图片及 14 个画板位置，按块同步。
- `boards/`：14 张图的 Mermaid 源码；与正文一起修改。
- `tutorial_feishu.xml`：由 `build_xml.py` 从正文生成的导入格式，不手工维护第二份正文。
- `images/`：真实硬件资料和脱敏 App 截图。封面插图不是产品实拍。

`build_xml.py` 需要 Python、beautifulsoup4 和 markdown-it-py，只转换本地文件，不发布到飞书。
生成后检查标题层级、代码块、表格、14 张画板和 2 张图片。在线更新前另读当前文档，保留原始副本，
逐块修改并回读；不要直接用整篇覆盖破坏已有资源或批注。

本次范围与检查记录见 `../../flow/tasks/2026-09-course-materials-review.md`。
代码使用课程 tag，修订材料在 main；训练营总入口和其他讲师内容不在本次编辑范围内。
