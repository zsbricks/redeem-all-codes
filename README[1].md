# Redeem All Codes

A Geode mod for Geometry Dash **2.2081 on Windows** that adds buttons inside the mod settings menu to update a GitHub-hosted code database and redeem known public vault / secret reward codes using `GameLevelManager::getGJSecretReward`.

## What it does

- Adds **Update code database** in Geode mod settings.
- Adds **Redeem database codes** in Geode mod settings.
- Downloads `codes.json` from a raw GitHub URL and caches it in the mod save folder.
- If the online database fails, it uses the cached database, then the built-in fallback list.
- Sends codes one at a time with a delay to avoid spamming requests.
- Lets you optionally enter your **player name** and **star count** for variable vault codes.

## What it does not do

- It does **not** bypass RobTop's servers.
- It does **not** unlock paid content.
- It does **not** force rewards that are already redeemed, expired, seasonal, puzzle-based, or locked behind progression.
- It does **not** scrape random websites automatically. You update `codes.json` in your GitHub repo.

## GitHub setup

```bash
git init
git add .
git commit -m "Initial commit"
git branch -M main
git remote add origin https://github.com/YOURNAME/redeem-all-codes.git
git push -u origin main
```

Then change the default **Database URL** setting in `mod.json` from:

```text
https://raw.githubusercontent.com/zBricks/redeem-all-codes/main/codes.json
```

to:

```text
https://raw.githubusercontent.com/YOURNAME/redeem-all-codes/main/codes.json
```

You can also change it in-game from the mod settings.

## Updating the database

Edit `codes.json`, add new public codes, then push to GitHub:

```bash
git add codes.json
git commit -m "Update code database"
git push
```

The next time you click **Update code database** or **Redeem database codes**, the mod downloads the newest raw `codes.json` and caches it.

Supported JSON shapes:

```json
{
  "version": "2026-05-07",
  "codes": ["lenny", "blockbite"]
}
```

or:

```json
{
  "version": "2026-05-07",
  "categories": {
    "vault": ["lenny", { "code": "blockbite" }]
  }
}
```

## Build

Install the Geode SDK and CLI first, then run:

```bash
geode build
```

Or with CMake:

```bash
cmake -B build
cmake --build build --config RelWithDebInfo
```

The `.geode` package will appear in the build output folder.

## Notes

This repo targets:

- Geode: `v5.6.1`
- Geometry Dash: `2.2081`
- Platform: Windows

If your installed Geode SDK version is different, update the `"geode"` field in `mod.json` to match your SDK.
