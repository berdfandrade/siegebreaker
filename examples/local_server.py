"""Servidor HTTP/1.1 de bancada, acessível em 127.0.0.1."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from time import sleep


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        if self.path == "/slow":
            sleep(0.2)
        self.send_response(503 if self.path == "/error" else 200)
        self.send_header("Content-Length", "3")
        if self.path == "/close":
            self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(b"ok\n")
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    print("Escutando em http://127.0.0.1:8080/", flush=True)
    ThreadingHTTPServer(("127.0.0.1", 8080), Handler).serve_forever()
