"""
Quick local server for usyseek mockup
Run with: python serve.py
Or simply open index.html directly in any modern web browser!
"""
import http.server
import socketserver
import webbrowser
import os

PORT = 5173

class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cache-Control', 'no-cache, no-store, must-revalidate')
        self.send_header('Pragma', 'no-cache')
        self.send_header('Expires', '0')
        super().end_headers()

def main():
    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    with socketserver.TCPServer(("", PORT), Handler) as httpd:
        url = f"http://localhost:{PORT}/index.html"
        print(f"\n=======================================================")
        print(f"  usyseek UI Mockup Server Running at:")
        print(f"  --> {url}")
        print(f"=======================================================\n")
        try:
            webbrowser.open(url)
        except Exception:
            pass
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nShutting down server.")

if __name__ == '__main__':
    main()
