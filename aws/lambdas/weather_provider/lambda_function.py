import boto3
import json
import os
import urllib.parse
import urllib.request

from datetime import datetime, timezone
from decimal import Decimal



OPEN_METEO_URL = "https://api.open-meteo.com/v1/forecast"

# Replace with the actual coordinates of the charging station.
STATION_LATITUDE = 19.510667
STATION_LONGITUDE = -99.125944

TIMEZONE = "America/Mexico_City"

STATUS_TABLE_NAME = os.environ.get(
    "STATUS_TABLE_NAME",
    "StationStatus"
)

STATION_ID = os.environ.get(
    "STATION_ID",
    "station_001"
)

dynamodb = boto3.resource("dynamodb")
status_table = dynamodb.Table(STATUS_TABLE_NAME)

def get_weather_data():
    params = {
        "latitude": STATION_LATITUDE,
        "longitude": STATION_LONGITUDE,
        "current": (
            "shortwave_radiation,"
            "cloud_cover,"
            "precipitation_probability"
        ),
        "timezone": TIMEZONE,
    }

    url = OPEN_METEO_URL + "?" + urllib.parse.urlencode(params)

    with urllib.request.urlopen(url, timeout=10) as response:
        data = json.loads(response.read().decode("utf-8"))

    current = data["current"]

    return {
        "timestamp": current["time"],
        "shortwave_radiation": current["shortwave_radiation"],
        "cloud_cover": current["cloud_cover"],
        "precipitation_probability": current[
            "precipitation_probability"
        ],
        "source": "open_meteo",
    }


def lambda_handler(event, context):
    try:
        weather = get_weather_data()

        print("Weather update:", json.dumps(weather))

        update_station_weather(weather)

        return {
            "success": True,
            "weather": weather,
        }

    except Exception as exc:
        print("Weather provider error:", str(exc))

        return {
            "success": False,
            "error": str(exc),
            "source": "open_meteo",
        }

def update_station_weather(weather):
    weather_item = {
        "timestamp": weather["timestamp"],
        "shortwave_radiation": Decimal(
            str(weather["shortwave_radiation"])
        ),
        "cloud_cover": Decimal(
            str(weather["cloud_cover"])
        ),
        "precipitation_probability": Decimal(
            str(weather["precipitation_probability"])
        ),
        "source": weather["source"],
        "retrieved_at": datetime.now(
            timezone.utc
        ).isoformat(),
    }

    status_table.update_item(
        Key={
            "station_id": STATION_ID
        },
        UpdateExpression="SET weather = :weather",
        ExpressionAttributeValues={
            ":weather": weather_item
        },
    )