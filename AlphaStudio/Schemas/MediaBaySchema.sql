-- AlphaStudio MediaBay & VST Sound Relational Catalog Schema (Zero Video)
CREATE TABLE IF NOT EXISTS media_assets (
    asset_id        INTEGER PRIMARY KEY AUTOINCREMENT,
    guid            TEXT NOT NULL UNIQUE,
    file_path       TEXT NOT NULL,
    asset_kind      TEXT NOT NULL CHECK (asset_kind IN ('AudioLoop', 'MidiLoop', 'VSTSound', 'TrackPreset', 'ProjectTemplate', 'PluginPreset')),
    display_name    TEXT NOT NULL,
    category        TEXT NOT NULL DEFAULT 'Uncategorized',
    sub_category    TEXT NOT NULL DEFAULT '',
    style_genre     TEXT NOT NULL DEFAULT '',
    instrument_tag  TEXT NOT NULL DEFAULT '',
    character_tag   TEXT NOT NULL DEFAULT '',
    musical_key     TEXT NOT NULL DEFAULT '',
    tempo_bpm       REAL NOT NULL DEFAULT 120.0,
    sample_rate     INTEGER NOT NULL DEFAULT 48000,
    channels        INTEGER NOT NULL DEFAULT 2,
    rating          INTEGER NOT NULL DEFAULT 0 CHECK (rating BETWEEN 0 AND 5),
    os_owner_user   TEXT NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_media_category ON media_assets(asset_kind, category, style_genre);
CREATE INDEX IF NOT EXISTS idx_media_rating   ON media_assets(rating DESC);
