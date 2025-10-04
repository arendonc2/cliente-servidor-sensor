#!/usr/bin/env python3
import sys, asyncio
import logging
import os, time  # MOD: usar variables de entorno y timestamp para escritura en TXT
from aiocoap import Context, Message, resource
from aiocoap.numbers.codes import Code

if sys.platform.startswith("win"):
    asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

DATAFILE = os.environ.get("COAP_DATAFILE", "/opt/coap/data.txt")  # MOD: ruta del TXT parametrizable por env

class DataSensor(resource.Resource):
    async def render_get(self, request):
        return Message(code=Code.CONTENT, payload=b"30C")

class EchoResource(resource.Resource):
    async def render_post(self, request):
        # MOD: guardar cada POST en el archivo TXT (append)
        payload_bytes = request.payload or b""
        payload_text = payload_bytes.decode("utf-8", "ignore")
        line = f"{time.strftime('%Y-%m-%dT%H:%M:%SZ')} payload={payload_text}\n"
        try:
            with open(DATAFILE, "a", encoding="utf-8") as f:
                f.write(line)
        except Exception as e:
            logging.exception("No pude escribir en %s: %s", DATAFILE, e)
        return Message(code=Code.CONTENT, payload=(b"echo: " + payload_bytes))

async def main():
    site = resource.Site()
    site.add_resource(["sensor"], DataSensor())
    site.add_resource(["echo"], EchoResource())
    # MOD: escuchar en todas las interfaces (no solo loopback)
    await Context.create_server_context(site, bind=("0.0.0.0", 5683))
    logging.info("CoAP server escuchando en coap://0.0.0.0:5683; datafile=%s", DATAFILE)  # MOD: log con datafile
    await asyncio.get_running_loop().create_future()

if __name__ == "__main__":
    asyncio.run(main())
