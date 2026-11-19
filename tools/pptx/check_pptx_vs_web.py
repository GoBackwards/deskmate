"""Check that a PPTX export still matches the web deck's slide text (catches a stale download).

usage: python check_pptx_vs_web.py deck.pptx deck_dir
Reports, per slide, text pieces from the HTML that are missing from the PPTX slide or its notes.
"""
import html
import json
import re
import sys
import zipfile
from html.parser import HTMLParser


class Text(HTMLParser):
    def __init__(self):
        super().__init__()
        self.body, self.notes, self.in_aside = [], [], False

    def handle_starttag(self, tag, attrs):
        if tag == "aside":
            self.in_aside = True

    def handle_endtag(self, tag):
        if tag == "aside":
            self.in_aside = False

    def handle_data(self, data):
        t = " ".join(data.split())
        if t:
            (self.notes if self.in_aside else self.body).append(t)


def norm(s):
    s = s.replace("’", "'").replace("‘", "'").replace("“", '"').replace("”", '"')
    return re.sub(r"\s+", "", s)


def pptx_text(z, name):
    if name not in z.namelist():
        return ""
    xml = z.read(name).decode("utf8")
    return html.unescape("".join(re.findall(r"<a:t>([^<]*)</a:t>", xml)))


def main():
    pptx, deck = sys.argv[1], sys.argv[2]
    order = json.load(open(f"{deck}/project/deck.json", encoding="utf8"))["order"]
    z = zipfile.ZipFile(pptx)
    ok = True
    for i, sid in enumerate(order, 1):
        p = Text()
        p.feed(open(f"{deck}/project/slides/{sid}.html", encoding="utf8").read())
        slide = norm(pptx_text(z, f"ppt/slides/slide{i}.xml"))
        notes = norm(pptx_text(z, f"ppt/notesSlides/notesSlide{i}.xml"))
        miss = [t for t in p.body if norm(t) not in slide]
        miss_notes = [t for t in p.notes if norm(t) not in notes]
        print(f"slide {i} ({sid}): {len(p.body) - len(miss)}/{len(p.body)} text pieces found; notes {'match' if not miss_notes else 'DIFFER'}")
        for t in miss:
            print("   missing:", t[:90])
        for t in miss_notes:
            print("   notes missing:", t[:90])
        ok = ok and not miss and not miss_notes
    print("PPTX matches the web deck" if ok else "PPTX differs from the web deck")


if __name__ == "__main__":
    main()
