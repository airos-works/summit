#!/usr/bin/env python3
"""Create PDF viewer fixtures and serve them with download/session policies.

Requires reportlab and pypdf. Run --generate-only for Poppler visual QA.
The HTTP server is local test infrastructure, not part of the browser.
"""
import argparse
import hashlib
import http.server
import json
import pathlib
import re
import textwrap
from urllib.parse import parse_qs, urlsplit


def generate(directory):
    from reportlab.pdfgen import canvas
    from reportlab.lib import colors
    from reportlab.lib.pagesizes import A4, landscape
    from reportlab.pdfbase import pdfmetrics
    from reportlab.pdfbase.ttfonts import TTFont
    from pypdf import PdfReader, PdfWriter
    directory.mkdir(parents=True, exist_ok=True)
    font = pathlib.Path('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf')
    pdfmetrics.registerFont(TTFont('FixtureSans', str(font)))
    output = directory / 'fixture.pdf'
    document = canvas.Canvas(str(output), pagesize=A4, invariant=1)
    document.setTitle('Summit PDF fixture')
    document.setAuthor('Summit native integration tests')
    for number in range(1, 4):
        width, height = landscape(A4) if number == 3 else A4
        document.setPageSize((width, height))
        document.bookmarkPage(f'page-{number}')
        document.addOutlineEntry(f'Page {number}', f'page-{number}', 0)
        document.setFillColor(colors.HexColor('#254c3e'))
        document.rect(0, height - 140, width, 140, fill=1, stroke=0)
        document.setFillColor(colors.white)
        document.setFont('FixtureSans', 25)
        document.drawString(44, height - 62, 'Summit PDF viewer')
        document.setFont('FixtureSans', 13)
        document.drawString(44, height - 94, f'PAGE {number} - native rendering fixture')
        document.setFillColor(colors.HexColor('#29382f'))
        document.setFont('FixtureSans', 14)
        text = document.beginText(44, height - 184)
        text.setLeading(23)
        paragraph = ('PDF_PAGE_' + str(number) + '_MARKER. This document checks selectable text, '
                     'embedded fonts, vector graphics, page navigation, zoom and exact downloads. '
                     'Every page has a different marker so the native test can verify navigation.')
        for line in textwrap.wrap(paragraph, 67 if number < 3 else 98):
            text.textLine(line)
        text.textLine(''); text.textLine('Unicode: café, naïve, Straße, Ελληνικά.')
        document.drawText(text)
        if number == 1:
            document.setFillColor(colors.HexColor('#dce9e4'))
            document.rect(44, 225, width - 88, 190, fill=1, stroke=0)
            path = document.beginPath(); path.moveTo(44, 225); path.lineTo(218, 390)
            path.lineTo(325, 270); path.lineTo(390, 344); path.lineTo(width - 44, 225); path.close()
            document.setFillColor(colors.HexColor('#54836a')); document.drawPath(path, fill=1, stroke=0)
            document.setFillColor(colors.HexColor('#254c3e')); document.setFont('FixtureSans', 12)
            document.drawString(44, 190, 'Vector artwork must remain sharp when zoomed.')
        elif number == 2:
            document.setFont('FixtureSans', 12)
            document.drawString(44, 440, 'A read-only PDF form value:')
            document.acroForm.textfield(name='reader-note', value='Visible form value',
                x=44, y=395, width=300, height=30, fieldFlags='readOnly',
                borderColor=colors.HexColor('#54836a'), fillColor=colors.HexColor('#f4f7f2'))
            document.drawString(44, 345, 'Jump to page 3')
            document.linkRect('', 'page-3', (44, 338, 190, 361), relative=0, thickness=0)
        else:
            document.setFont('FixtureSans', 14)
            for index, label in enumerate(['Text', 'Vectors', 'Search', 'Save']):
                x = 44 + index * 186
                document.setFillColor(colors.HexColor('#e1ebe5'))
                document.rect(x, 115, 174, 115, fill=1, stroke=0)
                document.setFillColor(colors.HexColor('#254c3e'))
                document.drawString(x + 14, 181, label)
                document.drawString(x + 14, 151, 'PASS fixture')
        document.setFillColor(colors.HexColor('#607165')); document.setFont('FixtureSans', 10)
        document.drawString(44, 38, 'Summit - air/OS PDF integration test')
        document.drawRightString(width - 44, 38, f'{number} / 3')
        document.showPage()
    document.save()
    source = PdfReader(output)
    assert len(source.pages) == 3 and source.get_fields()['reader-note']['/V'] == 'Visible form value'
    widgets = [a.get_object() for p in source.pages for a in p.get('/Annots', [])
               if a.get_object().get('/Subtype') == '/Widget']
    assert len(widgets) == 1 and widgets[0].get('/V') == 'Visible form value' and widgets[0]['/AP']['/N']
    for filename, scripted in [('password.pdf', False), ('scripted.pdf', True)]:
        writer = PdfWriter(); writer.append(source)
        writer.add_metadata({"/Title": "Summit PDF fixture", "/Author": "Summit native integration tests"})
        if scripted:
            writer.add_js("app.alert('SUMMIT_PDF_SCRIPT_SHOULD_NOT_RUN');")
        else:
            writer.encrypt('summit-test', owner_password='summit-fixture-owner', algorithm='RC4-128')
        with (directory / filename).open('wb') as stream:
            writer.write(stream)
    (directory / 'invalid.pdf').write_bytes(b'%PDF-1.7\nInvalid Summit PDF fixture\n')
    (directory / 'empty.pdf').write_bytes(b'')
    record = {p.name: {'bytes': p.stat().st_size, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
              for p in sorted(directory.glob('*.pdf'))}
    (directory / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--directory', type=pathlib.Path, default=pathlib.Path('.vm/issue38-fixture'))
    parser.add_argument('--port', type=int, default=8771)
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    record = generate(args.directory)
    print(json.dumps(record), flush=True)
    if args.generate_only:
        return

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            request = urlsplit(self.path)
            name = request.path.lstrip('/')
            if name == 'embedded':
                query = parse_qs(request.query)
                blocked = query.get('policy') == ['block']
                body = b'''<!doctype html><title>Embedded PDF fixture</title>
<style>html,body{margin:0;height:100%}object,embed,iframe{width:100%;height:100%;border:0}</style>
<script src="embedded.js" defer></script><body>'''
                self.send_response(200)
                self.send_header('Content-Type', 'text/html')
                # Blob policy containers inherit HTTP CSP headers. In particular,
                # frame-ancestors on webmail must not block its own PDF attachment.
                sources = "'none'" if blocked else "'self' blob:"
                self.send_header('Content-Security-Policy', "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
                    + "frame-src " + sources + "; object-src " + sources + "; frame-ancestors 'none'")
                self.end_headers(); self.wfile.write(body); return
            if name == 'embedded.js':
                body = b'''window.fixtureViolations=[];window.fixtureLoaded=false;
document.addEventListener('securitypolicyviolation',e=>fixtureViolations.push(e.effectiveDirective));
(async()=>{const q=new URLSearchParams(location.search),tag=q.get('tag')||'object';
const e=document.createElement(['object','embed','iframe'].includes(tag)?tag:'object');e.id='pdf';
e.onload=()=>{window.fixtureLoaded=true};
let url;
if(q.get('source')==='http-blocked')url='ancestor-blocked.pdf';
else if(q.get('source')==='html-blob')url=URL.createObjectURL(new Blob([
'<!doctype html><p id="local-blob">Local blob</p><script>window.forbiddenInlineScript=true</scr'+'ipt>'
],{type:'text/html'}));
else {const r=await fetch('fixture.pdf');url=URL.createObjectURL(await r.blob())}
e.type=q.get('source')==='html-blob'?'text/html':'application/pdf';
e[e.tagName==='OBJECT'?'data':'src']=url;document.body.append(e);window.fixtureReady=true;
})().catch(e=>window.fixtureError=String(e));'''
                self.send_response(200); self.send_header('Content-Type', 'text/javascript')
                self.end_headers(); self.wfile.write(body); return
            if name == 'webmail-preview':
                # Proton Mail's attachment preview: its CSP (frame-ancestors
                # leaves out the mail site itself), and an object whose
                # fallback content is an embed and a notice.
                inner = 'inner' in parse_qs(request.query)
                body = b'''<!doctype html><title>Webmail PDF preview</title>
<style>html,body{margin:0;height:100%}body{display:flex;flex-direction:column;font:14px sans-serif}
#status{padding:4px 8px;background:#eee}#root{flex:1 1 auto;display:flex;flex-direction:column}
object{width:100%;flex:1 1 auto;display:flex;flex-direction:column-reverse}embed{display:flex}
iframe{border:0;width:100%;flex:1 1 auto}</style>
<script src="webmail-preview.js" defer></script><body><div id="status">starting</div><div id="root"></div>'''
                self.send_response(200)
                self.send_header('Content-Type', 'text/html')
                self.send_header('Content-Security-Policy', "default-src 'self'; media-src 'self' blob:; connect-src 'self' blob:; "
                    + "script-src 'self' 'wasm-unsafe-eval' blob:; style-src 'self' 'unsafe-inline'; img-src http: https: data: blob: cid:; "
                    + "frame-src 'self' blob:; object-src 'self' blob:; child-src 'self' data: blob:; frame-ancestors "
                    + ("'self'" if inner else "https://calendar.invalid https://drive.invalid"))
                self.send_header('Cross-Origin-Opener-Policy', 'same-origin-allow-popups')
                self.end_headers(); self.wfile.write(body); return
            if name == 'webmail-preview.js':
                body = b'''window.preview={state:'starting',violations:[]};
document.addEventListener('securitypolicyviolation',e=>preview.violations.push(e.effectiveDirective));
const q=new URLSearchParams(location.search);
const show=t=>{document.getElementById('status').textContent=t;document.title='PDF preview: '+t};
const navigatorReport=()=>({pdfViewerEnabled:navigator.pdfViewerEnabled,plugins:[...navigator.plugins].map(p=>p.name),
mimeTypes:[...navigator.mimeTypes].map(m=>m.type+':'+m.enabledPlugin.name)});
(async()=>{
if(q.get('frame')==='1'&&!q.has('inner')){
const f=document.createElement('iframe');const p=new URLSearchParams(q);p.delete('frame');p.set('inner','1');
f.src='webmail-preview?'+p;document.getElementById('root').append(f);
setInterval(()=>{const r=f.contentWindow&&f.contentWindow.preview;if(r){window.preview=r;show('framed: '+r.state)}},250);return}
const r=await fetch('fixture.pdf');const contents=[new Uint8Array(await r.arrayBuffer())];
const url=q.get('source')==='http'?'fixture.pdf':URL.createObjectURL(new File(contents,'fixture.pdf',{type:'application/pdf'}));
// React builds the element off the document, children included, then inserts it.
await new Promise(r=>setTimeout(r,50));
const late=q.get('order')==='late';
const o=document.createElement('object');if(!late)o.data=url;
o.className='preview';o.type='application/pdf';o.title='fixture.pdf';
const e=document.createElement('embed');e.className='flex';e.src=url;e.type='application/pdf';
const p=document.createElement('p');p.textContent='This browser does not support previewing PDF documents. Please download the file.';
o.append(e,p);document.getElementById('root').append(o);if(late)o.data=url;
const report=()=>{const d=o.contentDocument;Object.assign(preview,{url,navigator:navigatorReport(),
fallbackShown:p.getClientRects().length>0,embedShown:e.getClientRects().length>0,
objectDocument:d?d.contentType:null,objectHeight:o.getBoundingClientRect().height});
preview.state=preview.fallbackShown||preview.embedShown?'fallback':preview.objectDocument?'shown':'waiting';
show(preview.state+' | pdfViewerEnabled='+navigator.pdfViewerEnabled+' plugins='+navigator.plugins.length+
' mimeTypes='+navigator.mimeTypes.length+' document='+preview.objectDocument+(preview.violations.length?' violations='+preview.violations:''))};
o.onload=report;setInterval(report,500);report();
})().catch(e=>{preview.state='error';preview.error=String(e);show('error: '+e)});'''
                self.send_response(200); self.send_header('Content-Type', 'text/javascript')
                self.end_headers(); self.wfile.write(body); return
            if name == 'start':
                body = b'<!doctype html><title>PDF session ready</title><a href="private.pdf">Private PDF</a>'
                self.send_response(200); self.send_header('Set-Cookie', 'summit-pdf-session=1; HttpOnly; SameSite=Strict; Path=/')
                self.send_header('Content-Type', 'text/html'); self.end_headers(); self.wfile.write(body); return
            if name == 'private.pdf' and 'summit-pdf-session=1' not in self.headers.get('Cookie', ''):
                self.send_error(403); return
            filename = 'fixture.pdf' if name in ('attachment.pdf', 'private.pdf', 'ancestor-blocked.pdf') else name
            if filename not in record:
                self.send_error(404); return
            body = (args.directory / filename).read_bytes()
            self.send_response(200); self.send_header('Content-Type', 'application/pdf')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            if name == 'ancestor-blocked.pdf':
                self.send_header('Content-Security-Policy', "frame-ancestors 'none'")
            run = parse_qs(request.query).get('run', [''])[0]
            download_name = 'summit-pdf-' + run + '.pdf' if re.fullmatch(r'[a-zA-Z0-9-]{1,64}', run) else 'summit-fixture.pdf'
            self.send_header('Content-Disposition', ('attachment' if name == 'attachment.pdf' else 'inline') + '; filename="' + download_name + '"')
            self.end_headers(); self.wfile.write(body)

    http.server.ThreadingHTTPServer(('0.0.0.0', args.port), Handler).serve_forever()


if __name__ == '__main__':
    main()
