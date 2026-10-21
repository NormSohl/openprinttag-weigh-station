// Build one print-ready HTML file from the quickref Markdown files, one ref
// per page. usage: node build.js <out.html> <a.md> <b.md> ...
const fs = require('fs');
const [out, ...files] = process.argv.slice(2);

const escHtml = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
function inline(s) {
  s = escHtml(s);
  s = s.replace(/`([^`]+)`/g, '<code>$1</code>');
  s = s.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>');
  s = s.replace(/(^|[^*])\*([^*\s][^*]*)\*/g, '$1<em>$2</em>');
  s = s.replace(/\[([^\]]+)\]\([^)]+\)/g, '$1');   // links print as their text
  return s;
}

function convert(md) {
  const lines = md.replace(/\r/g, '').split('\n');
  const html = [];
  let i = 0;
  const indentOf = (l) => l.length - l.trimStart().length;

  // A list starting at line i with marker indent `ind`; returns html.
  function list(ind) {
    const ordered = /^\d+\.\s/.test(lines[i].trim());
    const items = [];
    while (i < lines.length) {
      const l = lines[i];
      if (!l.trim()) {                        // blank: list continues only if next line is a deeper item
        const nx = lines[i + 1];
        if (nx && indentOf(nx) > ind && /^(\d+\.|-)\s/.test(nx.trim())) { i++; continue; }
        break;
      }
      const t = l.trim(), li = indentOf(l);
      const isItem = /^(\d+\.|-)\s/.test(t);
      if (isItem && li === ind) {
        items.push({ text: t.replace(/^(\d+\.|-)\s+/, ''), sub: '' });
        i++;
      } else if (isItem && li > ind) {
        items[items.length - 1].sub += list(li);
      } else if (!isItem && li > ind && items.length) {
        items[items.length - 1].text += ' ' + t;   // continuation line
        i++;
      } else break;
    }
    const tag = ordered ? 'ol' : 'ul';
    return `<${tag}>` + items.map((it) => `<li>${inline(it.text)}${it.sub}</li>`).join('') + `</${tag}>`;
  }

  while (i < lines.length) {
    const l = lines[i], t = l.trim();
    if (!t) { i++; continue; }
    if (/^\*Print this page/.test(t)) { i++; continue; }   // meta line, not for the printed page
    let m;
    if ((m = t.match(/^(#{1,3})\s+(.*)$/))) { html.push(`<h${m[1].length}>${inline(m[2])}</h${m[1].length}>`); i++; continue; }
    if (t.startsWith('|')) {
      const rows = [];
      while (i < lines.length && lines[i].trim().startsWith('|')) { rows.push(lines[i].trim()); i++; }
      const cells = (r) => r.replace(/^\||\|$/g, '').split('|').map((c) => c.trim());
      const head = cells(rows[0]), body = rows.slice(2).map(cells);
      html.push('<table><thead><tr>' + head.map((c) => `<th>${inline(c)}</th>`).join('') + '</tr></thead><tbody>' +
        body.map((r) => '<tr>' + r.map((c) => `<td>${inline(c)}</td>`).join('') + '</tr>').join('') + '</tbody></table>');
      continue;
    }
    if (/^(\d+\.|-)\s/.test(t)) { html.push(list(indentOf(l))); continue; }
    const para = [];
    while (i < lines.length && lines[i].trim() && !/^(#|\||\d+\.\s|-\s)/.test(lines[i].trim())) { para.push(lines[i].trim()); i++; }
    html.push(`<p>${inline(para.join(' '))}</p>`);
  }
  return html.join('\n');
}

const pages = files.map((f) => `<section class="page">\n${convert(fs.readFileSync(f, 'utf8'))}\n` +
  `<footer>Weigh Station quick reference &middot; full detail in the user manual</footer>\n</section>`);

fs.writeFileSync(out, `<!doctype html>
<html><head><meta charset="utf-8"><title>Weigh Station Quick References</title>
<style>
  @page { size: Letter; margin: 0.6in 0.7in; }
  * { box-sizing: border-box; }
  body { font-family: "Segoe UI", Arial, sans-serif; font-size: 13pt; line-height: 1.38; color: #111; margin: 0; }
  .page { break-after: page; position: relative; min-height: 9.6in; }
  .page:last-child { break-after: auto; }
  h1 { font-size: 22pt; margin: 0 0 14pt; padding-bottom: 6pt; border-bottom: 3px solid #1f7a3a; }
  h2 { font-size: 15pt; margin: 18pt 0 6pt; color: #1f7a3a; }
  p { margin: 6pt 0; }
  ol, ul { margin: 4pt 0 8pt; padding-left: 22pt; }
  li { margin: 4pt 0; }
  li > ul { margin: 2pt 0; }
  code { font-family: Consolas, monospace; font-size: 11.5pt; background: #eef2ee; padding: 0 3pt; border-radius: 3px; }
  table { border-collapse: collapse; width: 100%; margin: 6pt 0 10pt; font-size: 12pt; }
  th, td { border: 1px solid #999; padding: 5pt 7pt; text-align: left; vertical-align: top; }
  th { background: #e4efe6; }
  footer { position: absolute; bottom: 0; left: 0; right: 0; font-size: 9pt; color: #777; border-top: 1px solid #ccc; padding-top: 4pt; }
</style></head><body>
${pages.join('\n')}
</body></html>`);
console.log(`wrote ${out}: ${files.length} pages`);
