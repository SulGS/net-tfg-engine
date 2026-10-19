#pragma once

#include "GameState.hpp"
#include "Components.hpp"
#include <algorithm>
#include <vector>

// Geometry of the arena (tiles, pillars, edge walls and centre spokes), built ONCE here and used by both
// AsteroidShooterGame::InitECSLogic and AsteroidShooterGameRenderer::InitECSRenderer. The two sides must match one to
// one: what the server collides with is what each client draws (each wall/spoke reads its own state slot, see
// EdgeWallSlot), so every position and every (cellId, dir, half) comes from this single source. Each side only adds
// its own components (colliders and timers on the logic side, meshes and materials on the renderer side). The vectors
// keep the creation order of the original loops.
namespace ArenaLayout {

    constexpr float CELL_SIZE = 80.0f;               // tile side
    constexpr float PILLAR_SPACING = CELL_SIZE / 2;  // pillar grid step: one per tile corner and edge/side midpoint

    struct TileDef {
        int       cellId;
        glm::vec3 pos;
    };

    struct PillarDef {
        PillarID  id;
        glm::vec3 pos;
    };

    struct SpokeDef {
        int                   cellId;
        CellCardinalDirection dir;
        glm::vec3             pos;
        glm::vec3             rot;
    };

    struct Layout {
        std::vector<TileDef>   tiles;
        std::vector<PillarDef> pillars;
        std::vector<WallDef>   walls;
        std::vector<SpokeDef>  spokes;
    };

    inline int CellId(int cx, int cy) { return cx * MAP_SIZE + cy; }

    // World position (x or y) of pillar-grid index p, 0..2*MAP_SIZE.
    inline float PillarCoord(int p) { return (p - MAP_SIZE) * PILLAR_SPACING - PILLAR_SPACING; }

    // Centre of cell (cx, cy) in the XY plane.
    inline glm::vec2 CellCentre(int cx, int cy) { return glm::vec2(PillarCoord(2 * cx + 1), PillarCoord(2 * cy + 1)); }

    inline Layout Build() {
        Layout layout;

        // Tiles
        for (int x = 0; x < MAP_SIZE; x++)
            for (int y = 0; y < MAP_SIZE; y++)
                layout.tiles.push_back({ CellId(x, y),
                    glm::vec3((x - MAP_SIZE / 2.0f) * CELL_SIZE, (y - MAP_SIZE / 2.0f) * CELL_SIZE, -4.0f) });

        // Pillars: one per point of the (2*MAP_SIZE+1)^2 grid, tagged with every cell it borders.
        for (int px = 0; px <= 2 * MAP_SIZE; px++)
        {
            for (int py = 0; py <= 2 * MAP_SIZE; py++)
            {
                const bool isTileCentre = (px % 2 == 1 && py % 2 == 1);

                PillarID pid;
                if (isTileCentre)
                {
                    pid.addCell(CellId((px - 1) / 2, (py - 1) / 2));
                }
                else
                {
                    const int cx_min = (px - 2) / 2;
                    const int cx_max = px / 2;
                    const int cy_min = (py - 2) / 2;
                    const int cy_max = py / 2;

                    for (int cx = std::max(cx_min, 0); cx <= std::min(cx_max, MAP_SIZE - 1); cx++)
                    {
                        for (int cy = std::max(cy_min, 0); cy <= std::min(cy_max, MAP_SIZE - 1); cy++)
                        {
                            const bool onBorderX = (px == 2 * cx || px == 2 * cx + 1 || px == 2 * cx + 2);
                            const bool onBorderY = (py == 2 * cy || py == 2 * cy + 1 || py == 2 * cy + 2);
                            if (onBorderX && onBorderY)
                                pid.addCell(CellId(cx, cy));
                        }
                    }
                }
                layout.pillars.push_back({ pid, glm::vec3(PillarCoord(px), PillarCoord(py), -4.0f) });
            }
        }

        // Walls: one per 40-unit pillar gap, i.e. per HALF of a cell side (each side borders two subtiles and each half
        // toggles independently, with its own state slot, EdgeWallSlot).

        // Walls between horizontally adjacent pillars (px,py)→(px+1,py)
        for (int px = 0; px < 2 * MAP_SIZE; px++)
        {
            for (int py = 0; py <= 2 * MAP_SIZE; py++)
            {
                const float midX = (PillarCoord(px) + PillarCoord(px + 1)) / 2.0f;
                const float midY = PillarCoord(py);

                // An interior boundary matches TWO (cx,cy) combinations (one per side); edgeBuilt keeps only the first
                // so each physical half-edge is exactly one entity, not two independently-toggled ones.
                bool edgeBuilt = false;
                for (int cx = 0; cx < MAP_SIZE && !edgeBuilt; cx++)
                {
                    for (int cy = 0; cy < MAP_SIZE && !edgeBuilt; cy++)
                    {
                        if (px < 2 * cx || px >= 2 * cx + 2) continue;
                        if (py != 2 * cy && py != 2 * cy + 2) continue;

                        const CellCardinalDirection dir = (py == 2 * cy)
                            ? CellCardinalDirection::Down
                            : CellCardinalDirection::Up;
                        const bool onBorder = (py == 0) || (py == 2 * MAP_SIZE);

                        layout.walls.push_back({ cx, cy, dir,
                            glm::vec3(midX, midY, 0.0f),
                            glm::vec3(0.0f, 90.0f, 0.0f),
                            onBorder, px - 2 * cx });
                        edgeBuilt = true;
                    }
                }
            }
        }

        // Walls between vertically adjacent pillars (px,py)→(px,py+1)
        for (int px = 0; px <= 2 * MAP_SIZE; px++)
        {
            for (int py = 0; py < 2 * MAP_SIZE; py++)
            {
                const float midX = PillarCoord(px);
                const float midY = (PillarCoord(py) + PillarCoord(py + 1)) / 2.0f;

                bool edgeBuilt = false; // same reasoning as the loop above
                for (int cx = 0; cx < MAP_SIZE && !edgeBuilt; cx++)
                {
                    for (int cy = 0; cy < MAP_SIZE && !edgeBuilt; cy++)
                    {
                        if (py < 2 * cy || py >= 2 * cy + 2) continue;
                        if (px != 2 * cx && px != 2 * cx + 2) continue;

                        const CellCardinalDirection dir = (px == 2 * cx)
                            ? CellCardinalDirection::Left
                            : CellCardinalDirection::Right;
                        const bool onBorder = (px == 0) || (px == 2 * MAP_SIZE);

                        layout.walls.push_back({ cx, cy, dir,
                            glm::vec3(midX, midY, 0.0f),
                            glm::vec3(90.0f, 0.0f, 0.0f),
                            onBorder, py - 2 * cy });
                        edgeBuilt = true;
                    }
                }
            }
        }

        // Centre spokes: four per cell, from the cell centre to each side's midpoint (Down, Up, Left, Right).
        for (int cx = 0; cx < MAP_SIZE; cx++)
        {
            for (int cy = 0; cy < MAP_SIZE; cy++)
            {
                const int cellId = CellId(cx, cy);
                const glm::vec2 centre = CellCentre(cx, cy);

                const float edgeYdown = PillarCoord(2 * cy);
                const float edgeYup = PillarCoord(2 * cy + 2);
                const float edgeXleft = PillarCoord(2 * cx);
                const float edgeXright = PillarCoord(2 * cx + 2);

                layout.spokes.push_back({ cellId, CellCardinalDirection::Down,
                    glm::vec3(centre.x, (centre.y + edgeYdown) / 2.0f, 0.0f), glm::vec3(90.0f, 0.0f, 0.0f) });
                layout.spokes.push_back({ cellId, CellCardinalDirection::Up,
                    glm::vec3(centre.x, (centre.y + edgeYup) / 2.0f, 0.0f), glm::vec3(90.0f, 0.0f, 0.0f) });
                layout.spokes.push_back({ cellId, CellCardinalDirection::Left,
                    glm::vec3((centre.x + edgeXleft) / 2.0f, centre.y, 0.0f), glm::vec3(0.0f, 90.0f, 0.0f) });
                layout.spokes.push_back({ cellId, CellCardinalDirection::Right,
                    glm::vec3((centre.x + edgeXright) / 2.0f, centre.y, 0.0f), glm::vec3(0.0f, 90.0f, 0.0f) });
            }
        }

        return layout;
    }
}
