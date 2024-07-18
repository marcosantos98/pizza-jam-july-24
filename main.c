#define ARENA_IMPLEMENTATION
#include <arena.h>
#include <math.h>
#include <raylib.h>
#include <raymath.h>
#include <stdbool.h>
#include <stdint.h>

#if defined(PLATFORM_WEB)
#include <emscripten/emscripten.h>
#endif

static Arena arena = {0};

float Approach(float current, float target, float increase) {
    if (current < target) {
        return fmin(current + increase, target);
    }
    return fmax(current - increase, target);
}

typedef int32_t TextureID;
int32_t texture_cnt = 0;
#define MAX_TEX 5
Texture2D textures[MAX_TEX];

TextureID add_tex(const char *path) {
    TextureID id = texture_cnt++;
    textures[id] = LoadTexture(path);
    return id;
}

Texture2D get_tex(TextureID id) {
    return textures[id];
}

static Sound land;
static Sound jump;
static Sound pickup;

typedef struct Animation {
    TextureID tex;
    float speed;
    int frame_cnt;
    float timer;
    int current_frame;
} Animation;

void update_anim(Animation *anim, float dt) {
    anim->timer += dt;
    if (anim->timer > anim->speed) {
        anim->current_frame++;
        anim->timer = 0;
        if (anim->current_frame >= anim->frame_cnt) {
            anim->current_frame = 0;
        }
    }
}

void reset_anim(Animation *anim) {
    anim->current_frame = 0;
    anim->timer = 0;
}
// :en
typedef enum EntityProp {
    EP_NIL,
    EP_COLLIDABLE,
    EP_RIDABLE,
    EP_PLAT,
} EntityProp;

typedef enum EntityId {
    EID_NIL,
    EID_CHUNK,
    EID_DEAD_ZONE,
    EID_MOVING_PLAT,
    EID_PLAT,
    EID_JUMP_COFFEE,
    EID_CHECK_COFFEE,
    EID_PLAYER,
    EID_TROPHY,
    // :id
} EntityId;

#define MAX_PROPS 10
typedef struct Entity {
    Vector2 pos;
    Vector2 vel;
    Vector2 remainder;
    Vector2 size;
    Rectangle aabb;
    bool flip;
    struct Entity *last_collided;
    bool is_collidable;
    int32_t prop_cnt;
    EntityProp *props;
    bool grounded;
    EntityId id;
    Vector2 respawn;
    TextureID texId;
    bool is_valid;
    bool played_land;
    float fall_time;
    void *user_data;
} Entity;

void en_setup(Entity *en, float x, float y, float w, float h) {
    en->vel = (Vector2){0, 0};
    en->remainder = (Vector2){0, 0};
    en->pos = (Vector2){x, y};
    en->size = (Vector2){w, h};
    en->aabb = (Rectangle){x, y, w, h};
    en->is_valid = true;
    en->played_land = true;
    en->props = arena_alloc(&arena, sizeof(EntityProp) * MAX_PROPS);
    memset(en->props, 0, sizeof(EntityProp) * MAX_PROPS);
}

void en_add_props(Entity *en, EntityProp prop) {
    en->props[en->prop_cnt++] = prop;
}

bool en_has_prop(Entity *en, EntityProp prop) {
    if (en->prop_cnt <= 0)
        return false;
    for (int i = 0; i < MAX_PROPS; i++) {
        if (en->props[i] == prop)
            return true;
    }
    return false;
}

bool en_collides_with(Entity *en, Entity **collidables, size_t collidables_len, Vector2 at) {
    Rectangle to_check = {at.x, at.y, en->aabb.width, en->aabb.height};
    for (int i = 0; i < collidables_len; i++) {
        Entity *c = collidables[i];
        if (CheckCollisionRecs(c->aabb, to_check)) {
            en->last_collided = c;
            return true;
        }
    }
    return false;
}

typedef void (*Action)(Entity *);

int signd(int x) {
    return (x > 0) - (x < 0);
}

void ActorMoveX(Entity **collidables, size_t collidables_len, Entity *e, float amount, Action callback) {
    e->remainder.x += amount;
    int move = round(e->remainder.x);
    if (move != 0) {
        e->remainder.x -= move;
        int sign = signd(move);
        while (move != 0) {
            if (!en_collides_with(e, collidables, collidables_len, (Vector2){e->pos.x + sign, e->pos.y})) {
                e->pos.x += sign;
                move -= sign;
            } else {
                if (callback) {
                    callback(e);
                }
                if (e->last_collided && !e->last_collided->is_collidable) {
                    e->pos.x += sign;
                    move -= sign;
                } else {
                    break;
                }
            }
        }
    }
}

void ActorMoveY(Entity **collidables, size_t collidables_len, Entity *e, float amount, Action callback) {
    e->remainder.y += amount;
    int move = round(e->remainder.y);
    if (move != 0) {
        e->remainder.y -= move;
        int sign = signd(move);
        while (move != 0) {
            if (!en_collides_with(e, collidables, collidables_len, (Vector2){e->pos.x, e->pos.y + sign})) {
                e->pos.y += sign;
                move -= sign;
            } else {
                if (callback) {
                    callback(e);
                }
                if (e->last_collided != NULL && !e->last_collided->is_collidable) {
                    e->pos.y += sign;
                    move -= sign;
                } else {
                    if (e->vel.y > 0) {
                        if (!e->played_land) {
                            PlaySound(land);
                            e->played_land = true;
                        }
                        e->grounded = true;
                        e->fall_time = 0;
                    }
                    e->vel.y = 0;
                    break;
                }
            }
        }
    }
}

typedef enum Screen {
    S_MENU,
    S_GAME,
    S_LOST,
    S_WON,
    S_CREDITS,
    S_DIFFICULTY,
    S_HOWTO,
} Screen;

#define MAX_ENTITIES 1024
typedef struct Game {
    Screen screen;
    size_t en_cnt;
    Entity **ens;
} Game;

static Game game = {0};

// :player
typedef enum PlayerState {
    PS_IDLE,
    PS_WALK,
} PlayerState;

typedef struct Player {
    Animation *animation;
    PlayerState state;
    PlayerState prevState;
    float jump_power;
    float jump_boost_time;
} Player;

Entity *player_init() {
    Entity *e = arena_alloc(&arena, sizeof(Entity));
    memset(e, 0, sizeof(Entity));
    en_setup(e, 0, -24, 18, 24);
    e->user_data = arena_alloc(&arena, sizeof(Player));
    Player *data = (Player *)e->user_data;
    data->state = PS_IDLE;
    data->jump_power = -5;
    e->id = EID_PLAYER;
    return e;
}
void onCollide(Entity *self) {
    Player *data = (Player *)self->user_data;
    if (self->last_collided) {
        if (self->last_collided->id == EID_DEAD_ZONE) {
            if (data->jump_boost_time <= 0) {
                game.screen = S_LOST;
            } else {
                self->pos = self->respawn;
            }
        } else if (self->last_collided->id == EID_JUMP_COFFEE) {
            self->last_collided->is_valid = false;
            data->jump_power = -10;
            data->jump_boost_time = Clamp(data->jump_boost_time, data->jump_boost_time + 4, 20);
            PlaySound(pickup);
        } else if (self->last_collided->id == EID_CHECK_COFFEE) {
            self->respawn = (Vector2){self->pos.x, (self->pos.y + self->aabb.height - self->aabb.height)};
            self->last_collided->is_valid = false;
            PlaySound(pickup);
        } else if (self->last_collided->id == EID_TROPHY) {
            game.screen = S_WON;
        }
    }
}

void player_update(Entity *player, Animation *walk, Animation *idle) {
    player->aabb.x = player->pos.x + 4;
    player->aabb.y = player->pos.y;
    Player *data = (Player *)player->user_data;

    if (IsKeyDown(KEY_A)) {
        walk->speed = Approach(walk->speed, .1, 4 * GetFrameTime());
        player->vel.x = Approach(player->vel.x, -2.0, 22 * GetFrameTime());
        player->flip = true;
        data->state = PS_WALK;
    } else if (IsKeyDown(KEY_D)) {
        walk->speed = Approach(walk->speed, .1, 4 * GetFrameTime());
        player->vel.x = Approach(player->vel.x, 2.0, 22 * GetFrameTime());
        player->flip = false;
        data->state = PS_WALK;
    } else {
        data->state = PS_IDLE;
    }

    if (IsKeyPressed(KEY_SPACE) && player->grounded) {
        player->grounded = false;
        player->played_land = false;
        player->vel.y = data->jump_power;
        PlaySound(jump);
    }

    if (data->prevState != data->state) {
        switch (data->state) {
        case PS_IDLE:
            data->animation = idle;
            reset_anim(walk);
            walk->speed = .25;
            break;
        case PS_WALK:
            data->animation = walk;
            reset_anim(idle);
            break;
        default:
            break;
        }
        data->prevState = data->state;
    }

    if (!IsKeyDown(KEY_A) && !IsKeyDown(KEY_D)) {
        if (player->grounded) {
            player->vel.x = Approach(player->vel.x, 0.0, 10 * GetFrameTime());
        } else {
            player->vel.x = Approach(player->vel.x, 0.0, 12 * GetFrameTime());
        }
    }

    size_t collidables_len = 0;
    Entity *collidables[MAX_ENTITIES];

    for (int i = 0; i < game.en_cnt; i++) {
        Entity *c = game.ens[i];
        if (!c->is_valid) {
            continue;
        }
        bool hasProp = en_has_prop(c, EP_COLLIDABLE);
        if (c && hasProp) {
            collidables[collidables_len++] = c;
        }
    }

    ActorMoveX(collidables, collidables_len, player, player->vel.x, onCollide);
    player->vel.y = Approach(player->vel.y, 3.6, 13 * GetFrameTime());
    ActorMoveY(collidables, collidables_len, player, player->vel.y, onCollide);

    update_anim(data->animation, GetFrameTime());

    if (data->jump_boost_time > 0) {
        data->jump_boost_time -= GetFrameTime();
    } else if (data->jump_boost_time <= 0 && data->jump_power == -10) {
        data->jump_power /= 2;
    }
}
// ;player

Entity *en_collidable(float x, float y, float w, float h) {
    Entity *e = arena_alloc(&arena, sizeof(Entity));
    memset(e, 0, sizeof(Entity));
    e->is_collidable = true;
    en_setup(e, x, y, w, h);
    en_add_props(e, EP_COLLIDABLE);
    return e;
}

void en_move_y(Entity *en, float y) {
    en->pos.y = y;
    en->aabb.y = y;
}

void game_add_en(Game *game, Entity *e) {
    game->ens[game->en_cnt++] = e;
}

typedef enum PlatType {
    PT_ONE_WIDE,
    PT_TWO_WIDE,
    PT_THREE_WIDE,
    PT_FINAL = 5,
} PlatType;

Entity *gen_plat(float x, float y, PlatType type, TextureID texId) {
    Entity *e = arena_alloc(&arena, sizeof(Entity));
    memset(e, 0, sizeof(Entity));
    switch (type) {
    case PT_ONE_WIDE:
        en_setup(e, x, y, 16, 16);
        break;
    case PT_TWO_WIDE:
        en_setup(e, x, y, 16 * 2, 16);
        break;
    case PT_THREE_WIDE:
        en_setup(e, x, y, 16 * 3, 16);
        break;
    case PT_FINAL:
        en_setup(e, x, y, 16 * 6, 16 * 3);
        break;
    }
    en_add_props(e, EP_COLLIDABLE);
    en_add_props(e, EP_PLAT);
    e->id = EID_PLAT;
    e->texId = texId;
    e->is_collidable = true;
    return e;
}

void plat_render(Entity *self) {
    PlatType type = (self->aabb.width / 16) - 1;
    switch (type) {
    case PT_ONE_WIDE:
        DrawTextureRec(get_tex(self->texId), (Rectangle){8 * 16, 16, 16, 16}, (Vector2){self->pos.x, self->pos.y}, WHITE);
        break;
    case PT_TWO_WIDE:
        DrawTextureRec(get_tex(self->texId), (Rectangle){8 * 16, 16 * 3, 16, 16}, (Vector2){self->pos.x, self->pos.y}, WHITE);
        DrawTextureRec(get_tex(self->texId), (Rectangle){10 * 16, 16 * 3, 16, 16}, (Vector2){self->pos.x + 16, self->pos.y}, WHITE);
        break;
    case PT_THREE_WIDE:
        DrawTextureRec(get_tex(self->texId), (Rectangle){8 * 16, 16 * 3, 16, 16}, (Vector2){self->pos.x, self->pos.y}, WHITE);
        DrawTextureRec(get_tex(self->texId), (Rectangle){9 * 16, 16 * 3, 16, 16}, (Vector2){self->pos.x + 16, self->pos.y}, WHITE);
        DrawTextureRec(get_tex(self->texId), (Rectangle){10 * 16, 16 * 3, 16, 16}, (Vector2){self->pos.x + 32, self->pos.y}, WHITE);
        break;
    case PT_FINAL:
        for (int x = 0; x < 6; x++) {
            for (int y = 0; y < 3; y++) {
                DrawTextureRec(get_tex(self->texId), (Rectangle){(1 + x) * 16, (2 + y) * 16, 16, 16}, (Vector2){self->pos.x + (x * 16), self->pos.y + (y * 16)}, WHITE);
            }
        }
    }
}
Entity *gen_pickup(float x, float y, EntityId id) {
    Entity *e = arena_alloc(&arena, sizeof(Entity));
    memset(e, 0, sizeof(Entity));
    en_setup(e, x, y, 16, 16);
    e->id = id;
    en_add_props(e, EP_COLLIDABLE);
    return e;
}
bool btn(Rectangle play, const char *text) {
    bool clicked = false;
    Color color = WHITE;
    Color textColor = BLACK;
    if (CheckCollisionPointRec(GetMousePosition(), play)) {
        color = BLUE;
        textColor = WHITE;
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            clicked = true;
        }
    }
    DrawRectangleRounded(play, .2, 10, color);
    DrawText(text, play.x + (play.width / 2) - (MeasureText(text, 20) / 2), (play.y + play.height / 2) - 10, 20, textColor);
    return clicked;
}

Entity *player;
Player *data;
Animation walk;
Animation idle;
Camera2D cam;
Camera2D minimapCam;
Entity *dead_zone;
RenderTexture2D minimap;
bool inited;
TextureID coffee;
TextureID trophy;
TextureID tileset;
TextureID boyWalk;
TextureID boyIdle;
int diff;

void UpdateDrawFrame() {
    switch (game.screen) {
    case S_HOWTO:
    case S_DIFFICULTY:
    case S_LOST:
    case S_WON:
    case S_CREDITS:
        if (IsKeyPressed(KEY_ESCAPE)) {
            game.screen = S_MENU;
        }
        break;
    case S_MENU:
        break;
    case S_GAME: {
#ifdef Debug
        if (IsKeyPressed(KEY_C)) {
            data->jump_boost_time = 20;
        } else if (IsKeyPressed(KEY_K)) {
            en_move_y(player, -10300);
            cam.target = player->pos;
        }
#endif

        if (IsKeyPressed(KEY_ESCAPE)) {
            game.screen = S_MENU;
        }

        player_update(player, &walk, &idle);

        cam.target = Vector2Lerp(cam.target, player->pos, fabsf(player->vel.y) * GetFrameTime());

        float zoom = Clamp(cam.zoom - floorf(-player->vel.y) * 100 / 100, 1.0, 2.0);
        cam.zoom = Lerp(cam.zoom, zoom, 1 * GetFrameTime());
        en_move_y(dead_zone, player->respawn.y + player->aabb.height + 32);
        minimapCam = cam;

        for (int i = 0; i < game.en_cnt; i++) {
            Entity *en = game.ens[i];
            if (en->pos.y > dead_zone->pos.y) {
                en->is_valid = false;
            }
            if (!en->is_valid) {
                continue;
            }
            if (en->id == EID_TROPHY) {
                en_move_y(en, en->pos.y - sinf(GetTime() * 3));
            }
        }

        BeginTextureMode(minimap);
        {
            ClearBackground(BLANK);
            minimapCam.zoom = 1;
            BeginMode2D(minimapCam);
            {
                for (int i = 0; i < game.en_cnt; i++) {
                    Entity *en = game.ens[i];

                    if (!en->is_valid) {
                        continue;
                    }
                    switch (en->id) {
                    case EID_PLAT:
                        plat_render(en);
                        break;
                    case EID_JUMP_COFFEE:
                        DrawRectangleRec(en->aabb, WHITE);
                        break;
                    case EID_CHECK_COFFEE:
                        DrawRectangleRec(en->aabb, GREEN);
                        break;
                    default:
                        break;
                    }
                }
                DrawRectangleRec(player->aabb, RED);
            }
            EndMode2D();
        }
        EndTextureMode();
    } break;
    default:
        break;
    }

    BeginDrawing();
    {

        switch (game.screen) {
        case S_HOWTO:
            const char *howtoText = "Controls:\n"
                                    "A : Move left | D : Move right | Space : Jump\n"
                                    "Objective:\n"
                                    "Keep jumping until you get the gold trophy!\n"
                                    "\n"
                                    "Rules:\n"
                                    "Keep getting coffee (white cups) to increase the jump boost timer.\n"
                                    "If the jum boost bar runs out and you hit the dead zone the game ends.\n"
                                    "Green cups replaces the respawn point and moves the dead zone to it.";
            ClearBackground(BLACK);
            int cnt = 0;
            const char **splited = TextSplit(howtoText, '\n', &cnt);
            float y = GetScreenHeight() / 2 - cnt * 20;
            for (int i = 0; i < cnt; i++) {
                Color color = WHITE;
                float size = MeasureText(splited[i], 20);
                DrawText(splited[i], ((GetScreenWidth() - size)) * 0.5, y + i * 20, 20, color);
            }

            DrawRectangleRounded((Rectangle){12, 12, 35, 35}, .2, 10, BEIGE);
            DrawRectangleRounded((Rectangle){10, 10, 35, 35}, .2, 10, WHITE);
            DrawText("ESC", 12, 12, 10, BLACK);
            break;
        case S_DIFFICULTY: {
            ClearBackground(BLACK);

            Vector2 xyMid = (Vector2){GetScreenWidth() / 2, GetScreenHeight() / 2};

            if (!inited) {
                xyMid.x -= 140;
                xyMid.y -= 200;
                DrawRectangleRounded((Rectangle){xyMid.x + 2, xyMid.y + 2, 35, 35}, .2, 10, BEIGE);
                DrawRectangleRounded((Rectangle){xyMid.x, xyMid.y, 35, 35}, .2, 10, WHITE);
                DrawText("A", xyMid.x + 2, (xyMid.y) + 2, 10, BLACK);
                DrawText("Move left", (xyMid.x) + 2 + 45, (xyMid.y) + 2, 10, RAYWHITE);
                DrawRectangleRounded((Rectangle){xyMid.x + 2, xyMid.y + 2 + 45, 35, 35}, .2, 10, BEIGE);
                DrawRectangleRounded((Rectangle){xyMid.x, xyMid.y + 45, 35, 35}, .2, 10, WHITE);
                DrawText("S", (xyMid.x) + 2, (xyMid.y) + 2 + 45, 10, BLACK);
                DrawText("Move right", (xyMid.x) + 2 + 45, (xyMid.y) + 2 + 45, 10, RAYWHITE);
                DrawRectangleRounded((Rectangle){xyMid.x + 2, xyMid.y + 2 + 45 + 45, 85, 35}, .2, 10, BEIGE);
                DrawRectangleRounded((Rectangle){xyMid.x, xyMid.y + 45 + 45, 85, 35}, .2, 10, WHITE);
                DrawText("SPACE", (xyMid.x) + 2, (xyMid.y) + 2 + 45 + 45, 10, BLACK);
                DrawText("Jump", (xyMid.x) + 2 + 95, (xyMid.y) + 2 + 45 + 45, 10, RAYWHITE);

                DrawTexture(get_tex(coffee), (xyMid.x) + 2 + 95 + 45, (xyMid.y), WHITE);
                DrawText("+ jump boost", (xyMid.x) + 2 + 95 + 45 + 16 + 10, (xyMid.y) + 4, 10, RAYWHITE);
                DrawTexture(get_tex(coffee), (xyMid.x) + 2 + 95 + 45, (xyMid.y) + 45, GREEN);
                DrawText("checkpoint", (xyMid.x) + 2 + 95 + 45 + 16 + 10, (xyMid.y) + 4 + 45, 10, RAYWHITE);
                DrawTexture(get_tex(trophy), (xyMid.x) + 2 + 95 + 45, (xyMid.y) + 45 + 45, WHITE);
                DrawText("final objective", (xyMid.x) + 2 + 95 + 45 + 16 + 10 + 10, (xyMid.y) + 4 + 45 + 45, 10, RAYWHITE);

                xyMid.x += 140;
                xyMid.y += 200;

                Rectangle how_to_play = (Rectangle){xyMid.x - 200, xyMid.y, 400, 35};
                if (btn(how_to_play, "Continue")) {
                    inited = true;
                    xyMid = (Vector2){GetScreenWidth() / 2, GetScreenHeight() / 2};
                }
            } else {

                Rectangle easy = (Rectangle){xyMid.x - GetScreenWidth() * .2 / 2, xyMid.y, GetScreenWidth() * .2, 45};
                if (btn(easy, "Easy")) {
                    diff = -2000;
                    game_add_en(&game, gen_plat(0, 0, PT_THREE_WIDE, tileset));
                    game_add_en(&game, gen_plat(-100, 0, PT_THREE_WIDE, tileset));
                    game_add_en(&game, gen_pickup(-100, -16, EID_JUMP_COFFEE));
                    int y = -2000;
                    game_add_en(&game, gen_plat(0, y - 100, PT_FINAL, tileset));
                    game_add_en(&game, gen_pickup(38, y - 160, EID_TROPHY));
                    while (y < -100) {
                        int rnd = GetRandomValue(0, 2);
                        int rndX = GetRandomValue(-200, 200);
                        game_add_en(&game, gen_plat(rndX, y, rnd, tileset));

                        if (y % 3 == 0) {
                            game_add_en(&game, gen_plat(-rndX, y, rnd, tileset));
                        } else if (y % 5 == 0) {
                            game_add_en(&game, gen_pickup(rndX, y - 16, EID_JUMP_COFFEE));
                        } else if (y % 11 == 0) {
                            game_add_en(&game, gen_pickup(rndX, y - 16, EID_CHECK_COFFEE));
                        }
                        y += 16 * 4;
                    }

                    game.screen = S_GAME;
                }
                Rectangle medium = {xyMid.x - GetScreenWidth() * .2 / 2, xyMid.y + 55, GetScreenWidth() * .2, 45};
                if (btn(medium, "Medium")) {
                    diff = -5000;
                    game_add_en(&game, gen_plat(0, 0, PT_THREE_WIDE, tileset));
                    game_add_en(&game, gen_plat(-100, 0, PT_THREE_WIDE, tileset));
                    game_add_en(&game, gen_pickup(-100, -16, EID_JUMP_COFFEE));
                    int y = -5000;
                    game_add_en(&game, gen_plat(0, y - 100, PT_FINAL, tileset));
                    game_add_en(&game, gen_pickup(38, y - 160, EID_TROPHY));
                    while (y < -100) {
                        int rnd = GetRandomValue(0, 2);
                        int rndX = GetRandomValue(-200, 200);
                        game_add_en(&game, gen_plat(rndX, y, rnd, tileset));

                        if (y % 3 == 0) {
                            game_add_en(&game, gen_plat(-rndX, y, rnd, tileset));
                        } else if (y % 5 == 0) {
                            game_add_en(&game, gen_pickup(rndX, y - 16, EID_JUMP_COFFEE));
                        } else if (y % 11 == 0) {
                            game_add_en(&game, gen_pickup(rndX, y - 16, EID_CHECK_COFFEE));
                        }
                        y += 16 * 4;
                    }

                    game.screen = S_GAME;
                }
                Rectangle hard = {xyMid.x - GetScreenWidth() * .2 / 2, xyMid.y + 110, GetScreenWidth() * .2, 45};
                if (btn(hard, "Hard")) {
                    diff = -10000;
                    game_add_en(&game, gen_plat(0, 0, PT_THREE_WIDE, tileset));
                    game_add_en(&game, gen_plat(-100, 0, PT_THREE_WIDE, tileset));
                    game_add_en(&game, gen_pickup(-100, -16, EID_JUMP_COFFEE));
                    int y = -10000;
                    game_add_en(&game, gen_plat(0, y - 100, PT_FINAL, tileset));
                    game_add_en(&game, gen_pickup(38, y - 160, EID_TROPHY));
                    while (y < -100) {
                        int rnd = GetRandomValue(0, 2);
                        int rndX = GetRandomValue(-200, 200);
                        game_add_en(&game, gen_plat(rndX, y, rnd, tileset));

                        if (y % 3 == 0) {
                            game_add_en(&game, gen_plat(-rndX, y, rnd, tileset));
                        } else if (y % 5 == 0) {
                            game_add_en(&game, gen_pickup(rndX, y - 16, EID_JUMP_COFFEE));
                        } else if (y % 11 == 0) {
                            game_add_en(&game, gen_pickup(rndX, y - 16, EID_CHECK_COFFEE));
                        }
                        y += 16 * 4;
                    }

                    game.screen = S_GAME;
                }
            }

            DrawRectangleRounded((Rectangle){12, 12, 35, 35}, .2, 10, BEIGE);
            DrawRectangleRounded((Rectangle){10, 10, 35, 35}, .2, 10, WHITE);
            DrawText("ESC", 12, 12, 10, BLACK);
        } break;
        case S_MENU:
            ClearBackground(BLACK);
            Vector2 xyMid = (Vector2){GetScreenWidth() / 2, GetScreenHeight() / 2};
            Rectangle play = (Rectangle){xyMid.x - GetScreenWidth() * .2 / 2, xyMid.y, GetScreenWidth() * .2, 45};

            if (btn(play, "START GAME")) {
                game.screen = S_DIFFICULTY;
            }
            Rectangle credits = (Rectangle){play.x, play.y + play.height + 10, play.width, play.height};
            if (btn(credits, "CREDITS")) {
                game.screen = S_CREDITS;
            }
            Rectangle how_to_play = (Rectangle){credits.x, credits.y + credits.height + 10, credits.width, credits.height};
            if (btn(how_to_play, "HOW TO")) {
                game.screen = S_HOWTO;
            }
            break;
        case S_GAME: {
            ClearBackground(BLACK);

            BeginMode2D(cam);
            {
                DrawRectangleGradientV(-GetScreenWidth(), diff, GetScreenWidth() * 2, abs(diff) + 2000, BLACK, BLUE);

                DrawTexturePro(
                    get_tex(data->animation->tex),
                    (Rectangle){data->animation->current_frame * 48, 0, player->flip ? -24 : 24, 48},
                    (Rectangle){player->pos.x, player->pos.y - 24, 24, 48},
                    (Vector2){},
                    0,
                    WHITE);

#ifdef Debug
                DrawRectangleLinesEx(player->aabb, 1.0, GREEN);
#endif

                for (int i = 0; i < game.en_cnt; i++) {
                    Entity *en = game.ens[i];
                    if (!en->is_valid) {
                        continue;
                    }
                    switch (en->id) {
                    case EID_PLAT:
                        plat_render(en);
                        break;
                    case EID_JUMP_COFFEE:
                        DrawTextureV(get_tex(coffee), en->pos, WHITE);
                        break;
                    case EID_CHECK_COFFEE:
                        DrawTextureV(get_tex(coffee), en->pos, GREEN);
                        break;
                    case EID_TROPHY:
                        DrawTextureV(get_tex(trophy), en->pos, WHITE);
                        break;
                    case EID_DEAD_ZONE:
                        DrawRectangleV(en->pos, en->size, RED);
                    default:
                        break;
                    }
#ifdef Debug
                    for (int i = 0; i < en->prop_cnt; i++) {
                        EntityProp p = en->props[i];
                        switch (p) {
                        case EP_COLLIDABLE:
                            DrawRectangleLinesEx(en->aabb, 1.0, RED);
                            break;
                        default:
                            break;
                        }
                    }
#endif
                }
            }
            EndMode2D();

            const float MAX_JUMP_BOOST = 20;
            float yStart = GetScreenHeight() - 100;
            float xStart = (GetScreenWidth() - 400) * 0.5;
            DrawRectangleV((Vector2){xStart, yStart}, (Vector2){400, 25}, BROWN);
            float current_width = 400 * data->jump_boost_time / MAX_JUMP_BOOST;
            DrawRectangleV((Vector2){xStart, yStart}, (Vector2){current_width, 25}, DARKBROWN);
            DrawRectangleLinesEx((Rectangle){xStart, yStart, 400, 25}, 2, DARKBROWN);
            DrawText(
                "Jump Boost",
                (GetScreenWidth() / 2 - MeasureText("Jump Boost", GetFontDefault().baseSize * 2) / 2) - 2,
                (yStart - GetFontDefault().baseSize * 2) - 2,
                GetFontDefault().baseSize * 2,
                GRAY);
            DrawText(
                "Jump Boost",
                GetScreenWidth() / 2 - MeasureText("Jump Boost", GetFontDefault().baseSize * 2) / 2,
                yStart - GetFontDefault().baseSize * 2,
                GetFontDefault().baseSize * 2,
                WHITE);

            DrawTexturePro(
                minimap.texture,
                (Rectangle){0, 0, minimap.texture.width, -minimap.texture.height},
                (Rectangle){10, GetScreenHeight() - GetScreenHeight() / 4 - 10, GetScreenWidth() / 4, GetScreenHeight() / 4},
                (Vector2){},
                0,
                WHITE);
            DrawRectangleLinesEx((Rectangle){10, GetScreenHeight() - GetScreenHeight() / 4 - 10, GetScreenWidth() / 4, GetScreenHeight() / 4}, 2.0, BLACK);

            DrawFPS(10, 55);

            DrawRectangleRounded((Rectangle){12, 12, 35, 35}, .2, 10, BEIGE);
            DrawRectangleRounded((Rectangle){10, 10, 35, 35}, .2, 10, WHITE);
            DrawText("ESC", 12, 12, 10, BLACK);

        } break;
        case S_LOST:
            ClearBackground(BLACK);
            DrawRectangleRounded((Rectangle){12, 12, 35, 35}, .2, 10, BEIGE);
            DrawRectangleRounded((Rectangle){10, 10, 35, 35}, .2, 10, WHITE);
            DrawText("ESC", 12, 12, 10, BLACK);
            break;
        case S_WON: {
            ClearBackground(BLACK);

            Vector2 xyMid = {GetScreenWidth() / 2, GetScreenHeight() / 2};

            DrawText("WON!", xyMid.x, xyMid.y, 30, WHITE);

            DrawRectangleRounded((Rectangle){12, 12, 35, 35}, .2, 10, BEIGE);
            DrawRectangleRounded((Rectangle){10, 10, 35, 35}, .2, 10, WHITE);
            DrawText("ESC", 12, 12, 10, BLACK);
        } break;
        case S_CREDITS: {
            const char *creditsText = "Tileset: \n"
                                      "https://essssam.itch.io/rocky-roads\n"
                                      "Character: \n"
                                      "https://free-game-assets.itch.io/villagers-sprite-sheets-free-pixel-art-pack\n"
                                      "Cup of Coffe: \n"
                                      "https://skalding.itch.io/coffee-cup-001\n"
                                      "Trophies:\n"
                                      "https://buddy-games.itch.io/trophies-sprites\n"
                                      "Land, Jump Sounds:\n"
                                      "https://opengameart.org/content/12-player-movement-sfx\n";
            ClearBackground(BLACK);
            int cnt = 0;
            const char **splited = TextSplit(creditsText, '\n', &cnt);
            int y = GetScreenHeight() / 2 - cnt * 20;
            for (int i = 0; i < cnt; i++) {
                Color color = WHITE;
                if (i % 2 == 0) {
                    color = RED;
                }
                int size = MeasureText(splited[i], 20);
                DrawText(splited[i], (GetScreenWidth() - size) * 0.5, y + i * 20, 20, color);
            }

            DrawRectangleRounded((Rectangle){12, 12, 35, 35}, .2, 10, BEIGE);
            DrawRectangleRounded((Rectangle){10, 10, 35, 35}, .2, 10, WHITE);
            DrawText("ESC", 12, 12, 10, BLACK);
        } break;
        default:
            break;
        }
    }
    EndDrawing();
}

int main(void) {
    SetTraceLogLevel(LOG_WARNING);
    InitAudioDevice();
    InitWindow(1024, 576, "I'm drinking black coffee!");
    SetTargetFPS(60);
#ifdef Debug
    SetExitKey(KEY_Q);
#else
    SetExitKey(KEY_NULL);
#endif
    cam = (Camera2D){};
    cam.offset = (Vector2){GetScreenWidth() / 2, GetScreenHeight() / 2};
    cam.zoom = 2.0;

    minimapCam = cam;

    //: load
    boyIdle = add_tex("./assets/Boy_idle.png");
    boyWalk = add_tex("./assets/Boy_walk.png");
    tileset = add_tex("./assets/tileset_forest.png");
    coffee = add_tex("./assets/coffee.png");
    trophy = add_tex("./assets/gold.png");

    jump = LoadSound("./assets/jump.wav");
    land = LoadSound("./assets/land.wav");
    pickup = LoadSound("./assets/pop1.wav");

    idle = (Animation){
        .tex = boyIdle,
        .speed = 0.25,
        .frame_cnt = 4,
        .timer = 0,
    };

    walk = (Animation){
        .tex = boyWalk,
        .speed = 0.25,
        .frame_cnt = 6,
        .timer = 0,
    };

    //: init
    player = player_init();
    data = (Player *)player->user_data;
    player->respawn = (Vector2){0, -25};
    data->animation = &idle;
    data->jump_boost_time = 20;

    game.ens = arena_alloc(&arena, sizeof(Entity *) * MAX_ENTITIES);
    game.screen = S_MENU;

    dead_zone = en_collidable(-1000, 10, 2000, 16);
    dead_zone->is_collidable = false;
    dead_zone->id = EID_DEAD_ZONE;
    en_move_y(dead_zone, player->respawn.y + player->aabb.height + 16);
    game_add_en(&game, dead_zone);

    minimap = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());

    inited = false;
    diff = 0;
#if defined(PLATFORM_WEB)
    emscripten_set_main_loop(UpdateDrawFrame, 60, 1);
#else
    while (!WindowShouldClose()) {
        UpdateDrawFrame();
    }
#endif
    CloseAudioDevice();
    CloseWindow();
}