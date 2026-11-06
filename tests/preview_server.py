"""Local UI preview only; this does not control hardware."""
import json
from urllib.parse import urlsplit, parse_qs
from copy import deepcopy
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
config = {'enabled': True, 'loops': 0, 'actions': [dict(type='key',key=k,mods=0,hold=40,wait=500) for k in (30,31,32)]}
configs=[deepcopy(config) for _ in range(4)]
running = [False]*4
class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def reply(self, data, code=200):
        body=json.dumps(data).encode(); self.send_response(code); self.send_header('Content-Type','application/json'); self.send_header('Content-Length',str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        path=urlsplit(self.path).path; i=int(parse_qs(urlsplit(self.path).query).get('id',['0'])[0])
        if path=='/':
            body=(Path(__file__).parent.parent/'main/index.html').read_bytes(); self.send_response(200); self.send_header('Content-Type','text/html; charset=utf-8'); self.send_header('Content-Length',str(len(body))); self.end_headers(); self.wfile.write(body)
        elif path=='/api/config': self.reply(configs[i])
        elif path=='/api/status': self.reply(dict(usb=True,updating=False,logics=[dict(enabled=configs[j]['enabled'],running=running[j],step=1,completed=0,loops=configs[j]['loops']) for j in range(4)]))
        else: self.reply({'error':'not found'},404)
    def do_POST(self):
        global configs,running
        path=urlsplit(self.path).path; i=int(parse_qs(urlsplit(self.path).query).get('id',['0'])[0])
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if path=='/api/config': configs[i]=body; running[i]=False
        elif path=='/api/start': running[i]=configs[i]['enabled']
        elif path=='/api/start-all': running=[r or c['enabled'] for r,c in zip(running,configs)]
        elif path=='/api/stop': running[i]=False
        elif path=='/api/stop-all': running=[False]*4
        else: self.reply({'error':'not found'},404); return
        self.reply({'ok':True})
HTTPServer(('127.0.0.1',8766),Handler).serve_forever()
