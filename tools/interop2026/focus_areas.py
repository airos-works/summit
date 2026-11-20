"""Interpret focus-area keys emitted by labels.py for annual reporting."""

import re


def areas_by_year(labels):
    annual = {}
    for area, urls in labels.items():
        year = area.rpartition("/")[0]
        annual.setdefault(year, {})[area] = urls
    return annual


def display_area(area):
    return re.sub(r"(^|/)interop-\d{4}-", r"\1", area)
