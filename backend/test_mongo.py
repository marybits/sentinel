"""
Run this FIRST, by itself, before touching Flask.
It just proves the Atlas connection string works.

    python test_mongo.py

If it prints "Connected!" you're good. If it errors, read the
troubleshooting notes at the bottom of this file.
"""

import os
from pymongo import MongoClient
from pymongo.server_api import ServerApi
from dotenv import load_dotenv

load_dotenv()
MONGO_URI = os.environ.get(
    "MONGO_URI",
    "mongodb+srv://<user>:<password>@cluster0.xxxxx.mongodb.net/",
)

client = MongoClient(MONGO_URI, server_api=ServerApi("1"))

try:
    client.admin.command("ping")
    print("Connected!")

    db = client["sentinel"]
    result = db["alerts"].insert_one({"test": True, "msg": "hello from Julia's laptop"})
    print(f"Test doc inserted with id: {result.inserted_id}")

    # Clean up the test doc so it doesn't clutter the real collection
    db["alerts"].delete_one({"_id": result.inserted_id})
    print("Test doc cleaned up. MongoDB Atlas is ready to go.")

except Exception as e:
    print("Connection FAILED:", e)

# ----------------------------------------------------------------
# TROUBLESHOOTING
# ----------------------------------------------------------------
# "bad auth" / "authentication failed"
#   -> Wrong username/password, OR your password has a special
#      character (@ : / ? #) that needs URL-encoding.
#      Easiest fix: go to Database Access in Atlas, edit the user,
#      set a new password with only letters + numbers.
#
# Times out / hangs forever
#   -> Network Access doesn't have your current IP allowed.
#      Go to Network Access -> Add IP Address -> Allow Access
#      from Anywhere (0.0.0.0/0). Takes ~1 min to apply.
#
# "No module named 'pymongo'"
#   -> pip install pymongo --break-system-packages
#      (or just: pip install pymongo)
#
# ModuleNotFoundError: dnspython
#   -> pip install "pymongo[srv]" --break-system-packages