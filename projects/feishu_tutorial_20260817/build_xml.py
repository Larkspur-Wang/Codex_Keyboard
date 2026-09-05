"""Convert the reviewed Markdown to Feishu XML; never publishes remotely."""

from pathlib import Path
import re

from bs4 import BeautifulSoup, Comment
from markdown_it import MarkdownIt


ROOT = Path(__file__).resolve().parent


def build():
    markdown = ROOT.joinpath("draft.md").read_text()
    html = MarkdownIt("commonmark", {"html": True}).enable("table").render(markdown)
    soup = BeautifulSoup(html, "html.parser")
    # The opening quote is repository metadata, not learner-facing text.
    soup.find("blockquote").decompose()
    for comment in list(soup.find_all(string=lambda value: isinstance(value, Comment))):
        board = re.search(r"WB(\d+):", comment)
        photo = re.search(r"PHOTO(\d+):\s*(\S+)\s*(.*)", comment)
        if board:
            path = next(ROOT.joinpath("boards").glob(f"wb{int(board[1]):02d}_*.mmd"))
            tag = soup.new_tag("whiteboard", attrs={"type": "mermaid", "path": "@./boards/" + path.name})
            comment.replace_with(tag)
        elif photo:
            tag = soup.new_tag("img", attrs={"path": "@./images/" + photo[2], "caption": photo[3].strip()})
            comment.replace_with(tag)
        else:
            comment.extract()
    for heading in soup.find_all(re.compile(r"h[1-6]")):
        level = int(heading.name[1])
        heading.name = "title" if level == 1 else f"h{level - 1}"
    for tag in soup.find_all("strong"):
        tag.name = "b"
    for pre in soup.find_all("pre"):
        code = pre.code
        classes = code.get("class", [])
        pre["lang"] = next((x[9:] for x in classes if x.startswith("language-")), "text")
        code.attrs = {}
    for code in soup.find_all("code"):
        if code.parent.name != "pre":
            code.name = "em"
    for cell in soup.find_all(["td", "th"]):
        p = soup.new_tag("p")
        for child in list(cell.contents):
            p.append(child.extract())
        cell.append(p)
    for li in soup.find_all("li"):
        for p in li.find_all("p", recursive=False):
            p.unwrap()
    for link in soup.find_all("a"):
        if link.get("href", "").startswith("http"):
            link["type"] = "url-preview"
    output = "\n\n".join(str(x) for x in soup.contents if str(x).strip()) + "\n"
    assert len(soup.find_all("whiteboard")) == 14
    assert len(soup.find_all("img")) == 2
    assert len(soup.find_all("title")) == 1
    ROOT.joinpath("tutorial_feishu.xml").write_text(output)
    print("Generated tutorial_feishu.xml: 14 boards, 2 images; no remote writes.")


if __name__ == "__main__":
    build()
