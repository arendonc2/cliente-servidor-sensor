#!/usr/bin/env python3
import sys, asyncio, logging, os, time
from aiocoap import Context, Message, resource
from aiocoap.numbers.codes import Code

if sys.platform.startswith("win"):
    asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

DATAFILE    = os.environ.get("COAP_DATAFILE", "data.txt")          # historial (append)
CURRENTFILE = os.environ.get("COAP_CURRENTFILE", "current.txt")    # estado actual (overwrite)

def ensure_parent(path):
    d = os.path.dirname(path) or "."
    os.makedirs(d, exist_ok=True)

def atomic_write(path, text):
    ensure_parent(path)
    tmp = f"{path}.tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
        f.flush(); os.fsync(f.fileno())
    os.replace(tmp, path)

def append_history(text):
    ensure_parent(DATAFILE)
    with open(DATAFILE, "a", encoding="utf-8") as f:
        f.write(text + "\n")

def _read_last_line(path):
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            lines = f.read().splitlines()
            while lines and not lines[-1].strip():
                lines.pop()
            return lines[-1] if lines else None
    except FileNotFoundError:
        return None
    except Exception as e:
        logging.exception("Error leyendo %s: %s", path, e)
        return None

def extract_state(line):
    if not line:
        return None
    tag = "payload="
    i = line.find(tag)
    return line[i+len(tag):].strip() if i != -1 else line.strip()

class SensorResource(resource.Resource):
    async def render_get(self, request):
        # Lee el estado actual; si no existe, usa la última línea del historial
        current = _read_last_line(CURRENTFILE)
        if current is None:
            current = extract_state(_read_last_line(DATAFILE))
        return Message(code=Code.CONTENT, payload=(current or "NO_DATA").encode("utf-8"))

    async def render_put(self, request):
        # Actualiza el estado actual y guarda en historial con timestamp
        payload_text = (request.payload or b"").decode("utf-8", "ignore").strip()
        ts = time.strftime("%Y-%m-%dT%H:%M:%SZ")
        try:
            atomic_write(CURRENTFILE, payload_text + "\n")
            append_history(f"{ts} payload={payload_text}")
        except Exception as e:
            logging.exception("Error actualizando estado: %s", e)
            return Message(code=Code.INTERNAL_SERVER_ERROR, payload=b"WRITE_FAIL")
        return Message(code=Code.CHANGED, payload=b"UPDATED")

class EchoResource(resource.Resource):
    async def render_post(self, request):
        # Compatibilidad: POST también actualiza estado actual y historial
        payload_text = (request.payload or b"").decode("utf-8", "ignore").strip()
        ts = time.strftime("%Y-%m-%dT%H:%M:%SZ")
        try:
            atomic_write(CURRENTFILE, payload_text + "\n")
            append_history(f"{ts} payload={payload_text}")
        except Exception as e:
            logging.exception("No pude escribir archivos: %s", e)
            return Message(code=Code.INTERNAL_SERVER_ERROR, payload=b"WRITE_FAIL")
        return Message(code=Code.CONTENT, payload=f"echo: {payload_text}".encode())

async def main():
    site = resource.Site()
    site.add_resource(["sensor"], SensorResource())  # GET (lee), PUT (actualiza)
    site.add_resource(["echo"],   EchoResource())    # POST (actualiza)
    await Context.create_server_context(site, bind=("0.0.0.0", 5683))
    logging.info("CoAP server en coap://0.0.0.0:5683; datafile=%s current=%s", DATAFILE, CURRENTFILE)
    await asyncio.get_running_loop().create_future()

if __name__ == "__main__":
    asyncio.run(main())
PY
