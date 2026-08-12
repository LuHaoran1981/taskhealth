#!/usr/bin/env python3
"""Generate .docx files for soft copyright application (no external deps)."""

import zipfile
import os
import re
import html

DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(DIR)

# ── OpenXML templates ────────────────────────────────────────────────────────

DOCX_MIMETYPE = "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"
REL_MIMETYPE = "application/vnd.openxmlformats-package.relationships+xml"
XML_MIMETYPE = "application/xml"

CONTENT_TYPES = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="{rel_mime}"/>
  <Default Extension="xml" ContentType="{xml_mime}"/>
  <Override PartName="/word/document.xml" ContentType="{docx_mime}"/>
</Types>""".format(rel_mime=REL_MIMETYPE, xml_mime=XML_MIMETYPE, docx_mime=DOCX_MIMETYPE)

ROOT_RELS = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
</Relationships>"""

DOC_RELS = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>
</Relationships>"""

STYLES_XML = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
  <w:style w:type="paragraph" w:styleId="Normal">
    <w:name w:val="Normal"/>
    <w:pPr><w:spacing w:before="0" w:after="0" w:line="276" w:lineRule="auto"/></w:pPr>
    <w:rPr><w:rFonts w:ascii="SimSun" w:eastAsia="SimSun" w:hAnsi="SimSun"/>
          <w:sz w:val="24"/></w:rPr>
  </w:style>
  <w:style w:type="paragraph" w:styleId="Heading1">
    <w:name w:val="heading 1"/>
    <w:basedOn w:val="Normal"/>
    <w:rPr><w:b/><w:sz w:val="36"/></w:rPr>
  </w:style>
  <w:style w:type="paragraph" w:styleId="Heading2">
    <w:name w:val="heading 2"/>
    <w:basedOn w:val="Normal"/>
    <w:rPr><w:b/><w:sz w:val="30"/></w:rPr>
  </w:style>
  <w:style w:type="paragraph" w:styleId="Heading3">
    <w:name w:val="heading 3"/>
    <w:basedOn w:val="Normal"/>
    <w:rPr><w:b/><w:sz w:val="26"/></w:rPr>
  </w:style>
  <w:style w:type="paragraph" w:styleId="Code">
    <w:name w:val="Code"/>
    <w:basedOn w:val="Normal"/>
    <w:pPr><w:spacing w:line="240" w:lineRule="auto" w:before="0" w:after="0"/></w:pPr>
    <w:rPr><w:rFonts w:ascii="Courier New" w:eastAsia="Courier New" w:hAnsi="Courier New"/>
          <w:sz w:val="20"/></w:rPr>
  </w:style>
</w:styles>"""

DOC_START = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"
            xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
<w:body>"""

DOC_END = """</w:body></w:document>"""


def escape_xml(s):
    return html.escape(s, quote=False)


def make_run(text, bold=False, mono=False):
    """Create a <w:r> element with optional formatting."""
    rpr_parts = []
    if bold:
        rpr_parts.append('<w:b/>')
    if mono:
        rpr_parts.append('<w:rFonts w:ascii="Courier New" w:eastAsia="Courier New" w:hAnsi="Courier New"/>')
        rpr_parts.append('<w:sz w:val="20"/>')
    rpr = f'<w:rPr>{"".join(rpr_parts)}</w:rPr>' if rpr_parts else ''
    return f'<w:r>{rpr}<w:t xml:space="preserve">{escape_xml(text)}</w:t></w:r>'


def inline_format(text, default_mono=False, default_bold=False):
    """Parse **bold** and `code` spans, return list of <w:r> elements."""
    # Split on **bold** and `code` patterns
    pattern = r'(\*\*.+?\*\*|`.+?`)'
    parts = re.split(pattern, text)
    runs = []
    for part in parts:
        if part.startswith('**') and part.endswith('**'):
            runs.append(make_run(part[2:-2], bold=True, mono=default_mono))
        elif part.startswith('`') and part.endswith('`'):
            runs.append(make_run(part[1:-1], bold=default_bold, mono=True))
        elif part:
            runs.append(make_run(part, bold=default_bold, mono=default_mono))
    return runs


def make_para_with_runs(runs, style="Normal"):
    """Create a paragraph from pre-formatted runs."""
    runs_xml = ''.join(runs)
    return f'<w:p><w:pPr><w:pStyle w:val="{style}"/></w:pPr>{runs_xml}</w:p>'


def make_simple_para(text, style="Normal", bold=False, mono=False):
    """Create a simple paragraph (no inline formatting)."""
    runs = [make_run(text, bold=bold, mono=mono)]
    return make_para_with_runs(runs, style)


def make_hr():
    return '<w:p><w:pPr><w:pBdr><w:bottom w:val="single" w:sz="6" w:space="1" w:color="auto"/></w:pBdr></w:pPr></w:p>'


def make_table(headers, rows):
    """Create a simple table."""
    parts = ['<w:tbl><w:tblPr><w:tblBorders>'
             '<w:top w:val="single" w:sz="4" w:space="0" w:color="auto"/>'
             '<w:left w:val="single" w:sz="4" w:space="0" w:color="auto"/>'
             '<w:bottom w:val="single" w:sz="4" w:space="0" w:color="auto"/>'
             '<w:right w:val="single" w:sz="4" w:space="0" w:color="auto"/>'
             '<w:insideH w:val="single" w:sz="4" w:space="0" w:color="auto"/>'
             '<w:insideV w:val="single" w:sz="4" w:space="0" w:color="auto"/>'
             '</w:tblBorders></w:tblPr>']

    parts.append('<w:tr>')
    for h in headers:
        runs = inline_format(h, default_bold=True)
        parts.append('<w:tc>' + make_para_with_runs(runs) + '</w:tc>')
    parts.append('</w:tr>')

    for row in rows:
        parts.append('<w:tr>')
        for cell in row:
            runs = inline_format(cell)
            parts.append('<w:tc>' + make_para_with_runs(runs) + '</w:tc>')
        parts.append('</w:tr>')

    parts.append('</w:tbl>')
    return ''.join(parts)


def convert_markdown_to_docx_body(md_path):
    """Convert markdown file to docx body XML."""
    with open(md_path, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    parts = []
    i = 0
    in_code_block = False
    in_table = False
    table_headers = []
    table_rows = []

    while i < len(lines):
        line = lines[i].rstrip('\n')

        # code block
        if line.startswith('```'):
            in_code_block = not in_code_block
            i += 1
            continue

        if in_code_block:
            parts.append(make_simple_para(line, "Code", mono=True))
            i += 1
            continue

        # table
        if '|' in line and line.strip().startswith('|'):
            cells = [c.strip() for c in line.strip().split('|')][1:-1]
            if not in_table:
                if all(re.match(r'^:?-{3,}:?$', c) for c in cells):
                    i += 1
                    continue
                in_table = True
                table_headers = cells
                table_rows = []
            else:
                table_rows.append(cells)
            i += 1
            continue
        elif in_table:
            parts.append(make_table(table_headers, table_rows))
            in_table = False
            table_headers = []
            table_rows = []

        # headings
        if line.startswith('# '):
            runs = inline_format(line[2:], default_bold=True)
            parts.append(make_para_with_runs(runs, "Heading1"))
        elif line.startswith('## '):
            runs = inline_format(line[3:], default_bold=True)
            parts.append(make_para_with_runs(runs, "Heading2"))
        elif line.startswith('### '):
            runs = inline_format(line[4:], default_bold=True)
            parts.append(make_para_with_runs(runs, "Heading3"))
        elif line.startswith('#### '):
            runs = inline_format(line[5:], default_bold=True)
            parts.append(make_para_with_runs(runs, "Heading3"))

        # horizontal rule
        elif line.strip() == '---':
            parts.append(make_hr())

        # bullet list
        elif line.startswith('- ') or line.startswith('* '):
            runs = inline_format(line[2:])
            parts.append(make_para_with_runs(runs, "ListBullet"))

        # numbered list
        elif re.match(r'^\d+\.\s', line):
            text = re.sub(r'^\d+\.\s', '', line)
            runs = inline_format(text)
            parts.append(make_para_with_runs(runs, "ListNumber"))

        # empty line → skip
        elif line.strip() == '':
            pass

        # normal paragraph
        else:
            runs = inline_format(line)
            parts.append(make_para_with_runs(runs))

        i += 1

    if in_table:
        parts.append(make_table(table_headers, table_rows))

    return ''.join(parts)


def convert_source_txt_to_docx_body(txt_path):
    """Convert source code text to docx body (monospace)."""
    with open(txt_path, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    parts = []
    for line in lines:
        line = line.rstrip('\n').rstrip('\r')
        if line.strip() == '':
            continue
        if (line.strip().startswith('软件名称:') or
              line.strip().startswith('版本号:') or
              line.strip().startswith('第 ') or
              (line.startswith('──') and line.endswith('──'))):
            parts.append(make_simple_para(line, "Code", bold=True, mono=True))
        else:
            parts.append(make_simple_para(line, "Code", mono=True))

    return ''.join(parts)


def create_docx(output_path, body_xml):
    """Create a .docx file from body XML."""
    with zipfile.ZipFile(output_path, 'w', zipfile.ZIP_DEFLATED) as zf:
        zf.writestr('[Content_Types].xml', CONTENT_TYPES)
        zf.writestr('_rels/.rels', ROOT_RELS)
        zf.writestr('word/_rels/document.xml.rels', DOC_RELS)
        zf.writestr('word/styles.xml', STYLES_XML)
        zf.writestr('word/document.xml', DOC_START + body_xml + DOC_END)
    print(f"  生成: {output_path}")


# ── Main ─────────────────────────────────────────────────────────────────────

if __name__ == '__main__':
    os.chdir(ROOT)

    md_path = os.path.join(DIR, '用户手册.md')
    if os.path.exists(md_path):
        body = convert_markdown_to_docx_body(md_path)
        create_docx(os.path.join(DIR, '用户手册.docx'), body)

    txt_path = os.path.join(DIR, 'taskhealth_source_code.txt')
    if os.path.exists(txt_path):
        body2 = convert_source_txt_to_docx_body(txt_path)
        create_docx(os.path.join(DIR, 'taskhealth_source_code.docx'), body2)

    print("\n完成。")
