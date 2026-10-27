#!/usr/bin/env python3
"""Local protocol regression fixture; no external model or credential is used."""
import argparse
import hashlib
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def fingerprint(text):
    return int.from_bytes(hashlib.sha256(text.encode('utf-8')).digest()[:3], 'big')


def serve(port, events, dimension):
    lock = threading.Lock()
    attempts = {}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            status = 200
            if "/timeout/" in self.path:
                time.sleep(2)
            with lock, open(events, 'a', encoding='utf-8') as stream:
                attempts[self.path] = attempts.get(self.path, 0) + 1
                attempt = attempts[self.path]
                stream.write(json.dumps({'path': self.path, 'body': body,
                    'has_authorization': bool(self.headers.get('Authorization'))}, ensure_ascii=False) + '\n')
            if '/http-error/' in self.path or ('/fail-second/' in self.path and attempt == 2):
                status, response = 400, {'error': {'message': 'regression fixture error'}}
            elif self.path.endswith('/embeddings') or self.path.endswith('/embed'):
                inputs = body['input']
                assert isinstance(inputs, list) and all(isinstance(x, str) for x in inputs)
                size = body.get('dimensions', dimension)
                vectors = [[float(fingerprint(text))] + [0.25] * (size - 1) for text in inputs]
                if '/bad-dimension/' in self.path:
                    vectors[0].append(1)
                if self.path.endswith('/embed'):
                    response = {'embeddings': vectors}
                else:
                    data = [{'index': i, 'embedding': vector} for i, vector in enumerate(vectors)]
                    if '/duplicate-index/' in self.path:
                        data[-1]['index'] = data[0]['index']
                    response = {'data': list(reversed(data))}
            elif self.path.endswith('/rerank'):
                docs, query = body['documents'], body['query']
                response = {'results': list(reversed([
                    {'index': i, 'relevance_score': 1 if query in text else 0,
                     'document': {'text': text}} for i, text in enumerate(docs)]))}
            elif self.path.endswith('/generate'):
                assert body.get('stream') is False
                response = {'response': body['prompt'], 'done': True}
            elif self.path.endswith('/completions'):
                text = body['messages'][-1]['content']
                response = {'choices': [{'message': {'content': text}}]}
                if '/empty/' in self.path:
                    response['choices'][0]['message']['content'] = ''
            else:
                status, response = 404, {'error': 'unknown mock route'}
            payload = json.dumps(response, ensure_ascii=False).encode('utf-8')
            if '/malformed/' in self.path:
                payload = b'{'
            if '/wrong-root/' in self.path:
                payload = b'[]'
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            try:
                self.wfile.write(payload)
            except BrokenPipeError:
                pass

    ThreadingHTTPServer(('0.0.0.0', port), Handler).serve_forever()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=8000)
    parser.add_argument('--events', required=True)
    parser.add_argument('--dimension', type=int, default=768)
    args = parser.parse_args()
    serve(args.port, args.events, args.dimension)
