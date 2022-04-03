/*
  Shootzilla
  Copyright (C) 2022 Bernhard Schelling

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include <ZL_Application.h>
#include <ZL_Display3D.h>
#include <ZL_Audio.h>
#include <ZL_Font.h>
#include <ZL_Scene.h>
#include <ZL_Input.h>
#include <ZL_Particles.h>
#include <ZL_SynthImc.h>

static ZL_Mesh MeshGround, MeshWall, MeshBullet, MeshSpider, MeshBat, MeshGhost;
#ifdef ZILLALOG
static ZL_Mesh MeshDbgCollision, MeshDbgSphere;
#endif
static ZL_RenderList RenderListMap, RenderList, *RenderLists[] = { &RenderListMap, &RenderList };
static ZL_Camera Camera;
static ZL_Light LightSun, LightPlayer, *Lights[] = { &LightSun, &LightPlayer };
static ZL_ParticleEmitter ParticleDamage, ParticleDestroy;
static ZL_Font fntMain, fntBig, fntTitle;
static ZL_Surface srfCrosshair;
static ZL_Sound sndBullet, sndHit, sndHit2, sndJump;
static ZL_SynthImcTrack imcMusic;

static bool IsTitle = true;
static int wave, wavespawns, kills;
static ticks_t waveticks, gameover;

enum { MAXMAPSIZE = 17, MAPW = 17, MAPH = 17 };
static char Map[MAXMAPSIZE*MAXMAPSIZE+1];
static float MapHeights[MAXMAPSIZE*MAXMAPSIZE+1];
enum { TILE_EMPTY = ' ', TILE_WALL = '#' };

const float SPEED_PITCH = 0.01f;
const float SPEED_YAW = 0.01f;
const float SPEED_ACCEL = 10.0f;
const float SPEED_AIRACCEL = 1.0f;
const float SPEED_FORWARD = 3.0f;
const float SPEED_STRAFE = 3.0f;
const float JUMP_STRENGTH = 3.0f;
const float SPEED_GRAV = -7.0f; //-9.8f;
const float PLAYER_RADIUS = 0.25f;
const float CAN_STEP_HEIGHT = 0.2f;
const float VIEW_HEIGHT = 0.42f;
const float WEAPON_DELAY = 0.1f;
const float BULLET_SPEED = 10.0f;

struct Thing
{
	enum Type { BULLET, PLAYER, ENEMY_SPIDER, ENEMY_BAT, ENEMY_GHOST, WORLD };
	Thing(Type t, float r) : type(t), radius(r) {}
	Type type;
	float radius;
	ZL_Matrix mtx;
	ZL_Vector3 vel;
};
struct Bullet : Thing
{
	Bullet() : Thing(BULLET, 0.1f) {}
};
struct Player : Thing
{
	Player() : Thing(PLAYER, 0.25f) {}
	ZL_Vector3 dir;
	float weapontimer = 0, maxhealth = 100, health = 100;
	ticks_t lasthit = 0;
	int jumps = 2;
};
struct Enemy : Thing
{
	Enemy(Type t, float r, float movspd, float atkdmg, float atkspd, float hlth) : Thing(t, r), movespeed(movspd), attackdamage(atkdmg), attackspeed(atkspd), health(hlth) {}
	ZL_Vector movetarget;
	float movespeed, attackdamage, attackspeed, attacktimer = 0, health;
};
struct EnemySpider : Enemy
{
	EnemySpider() : Enemy(ENEMY_SPIDER, 0.25f, RAND_RANGE(1.1f, 1.9f), RAND_RANGE(8,13), .5f, RAND_RANGE(.1, 1.5)) {}
	ZL_Vector move;
	ZL_Vector movetarget;
};
struct EnemyBat : Enemy
{
	EnemyBat() :    Enemy(ENEMY_BAT,    0.25f,  RAND_RANGE(1.5f, 2.5f), RAND_RANGE(11,15), .4f, RAND_RANGE(.9, 2.5)) {}
};
struct EnemyGhost : Enemy
{
	EnemyGhost() :  Enemy(ENEMY_GHOST,  0.5f, RAND_RANGE(2.1f, 3.6f)+wave*0.05f, RAND_RANGE(13,20), .25f, RAND_RANGE(2, 9)) {}
};

static struct World : Thing { World() : Thing(WORLD, 0) {} } world;

static Player player;
static std::vector<Bullet> bullets;
static std::vector<Enemy> enemies;

static void FadeWalls(float h)
{
	ZL_SeededRand rnd((unsigned)wave);
	RenderListMap.Reset();
	RenderListMap.Add(MeshGround, ZL_Matrix::Identity);
	for (int y = 0; y != MAPH; y++)
	for (int x = 0; x != MAPW; x++)
	{
		int i = y*MAPW+x;
		if (x == 0 || x == MAPW-1 || y == 0 || y == MAPH-1)
			RenderListMap.Add(MeshWall, ZL_Matrix::MakeRotateTranslate(ZL_Quat::FromRotateZ(0.01f*(PIHALF*(i%4))), ZLV3(x+.5f, y+.5f, MapHeights[i])));
		else if (Map[i] == TILE_WALL)
		{
			MapHeights[i] = rnd.Range(0.2f, 0.8f) - 1 + h;
			RenderListMap.Add(MeshWall, ZL_Matrix::MakeRotateTranslate(ZL_Quat::FromRotateZ(RAND_VARIATION(0.01)*(PIHALF*RAND_INT_MAX(3))), ZLV3(x+.5f, y+.5f, MapHeights[i])));
		}
	}
}

static void SpawnEnemy()
{
	float enemytype = RAND_FACTOR * (wave <= 2 ? .6f : (wave <= 4 ? .9f : 1.f)) + (wave/15.0f);
	Thing::Type etype = (enemytype < .6f ? Thing::ENEMY_SPIDER : (enemytype < .9f ? Thing::ENEMY_BAT : Thing::ENEMY_GHOST));
	ZL_Vector3 epos;
	for (;;)
	{
		switch (etype)
		{
			case Thing::ENEMY_SPIDER:
				epos = ZLV3(1.5f + 2.0f*RAND_INT_MAX(MAPW/2-1), 1.5f + 2.0f*RAND_INT_MAX(MAPH/2-1), .15f);
				break;
			case Thing::ENEMY_BAT:
				epos = ZLV3(RAND_RANGE(2, MAPW-2), RAND_RANGE(2, MAPH-2), RAND_RANGE(1.5, 2.5));
				break;
			case Thing::ENEMY_GHOST:
				epos = ZLV3(RAND_RANGE(2, MAPW-2), RAND_RANGE(2, MAPH-2), RAND_RANGE(1.7, 2.9));
				break;
			default:break;
		}
		if (epos.ToXY().GetDistanceSq(player.mtx.GetTranslateXY()) > (5.f*5.f))
			break; // don't spawn close to the player
	}
	switch (etype)
	{
		case Thing::ENEMY_SPIDER:
		{
			EnemySpider e;
			e.mtx.SetTranslate(epos);
			enemies.push_back(e);
			break;
		}
		case Thing::ENEMY_BAT:
		{
			EnemyBat e;
			e.mtx.SetTranslate(epos);
			enemies.push_back(e);
			break;
		}
		case Thing::ENEMY_GHOST:
		{
			EnemyGhost e;
			e.mtx.SetTranslate(epos);
			enemies.push_back(e);
			break;
		}
		default:break;
	}
}

static void StartWave()
{
	//MAPW = MAPH = newmapsz;
	Map[MAPW*MAPH] = '\0';
	memset(Map, TILE_WALL, MAPW*MAPH);

	for (int i = 0; i != 10; i++)
	{
		int emptyX = 1+2*RAND_INT_MAX(MAPW/2-1);//RAND_INT_RANGE(2, MAPW-3);
		int emptyY = 1+2*RAND_INT_MAX(MAPH/2-1);//RAND_INT_RANGE(2, MAPH-3);
		Map[emptyX*MAPW+emptyY] = TILE_EMPTY;
	}

	for (char empty = 0; empty < 4 - MIN(4/2, 2); empty++)
	{
		int currentx = MAPW/2|1, currenty = MAPH/2|1;
		for (int y = currenty - 2; y <= currenty + 2; y++)
			for (int x = currentx - 2; x <= currentx + 2; x++)
				Map[y*MAPW+x] = empty;
 
		REGENERATE:
		for (int i = 0; i != 100; i++)
		{
			int oldx = currentx, oldy = currenty;
			switch (RAND_INT_MAX(3))
			{
				case 0: if (currentx < MAPW-2) currentx += 2; break;
				case 1: if (currenty < MAPH-2) currenty += 2; break;
				case 2: if (currentx >      2) currentx -= 2; break;
				case 3: if (currenty >      2) currenty -= 2; break;
			}
			if (Map[currenty*MAPW+currentx] == empty) continue;
			Map[currenty*MAPW+currentx] = empty;
			Map[((currenty + oldy) / 2)*MAPW+((currentx + oldx) / 2)] = empty;
		}
 
		//check if all cells are visited
		for (int y = 1; y != MAPH; y += 2)
			for (int x = 1; x != MAPW; x += 2)
				if (Map[y*MAPW+x] > TILE_EMPTY) goto REGENERATE;
	}

	for (char& c : Map) if (c < TILE_EMPTY) c = TILE_EMPTY;

	//clear pillars with nothing around
	for (int y = 2; y != MAPH - 1; y+=2)
		for (int x = 2; x != MAPW - 1; x+=2)
			if (Map[y*MAPW+x] > TILE_EMPTY && Map[y*MAPW+x-1] <= TILE_EMPTY && Map[y*MAPW+x+1] <= TILE_EMPTY && Map[y*MAPW-MAPW+x] <= TILE_EMPTY && Map[y*MAPW+MAPW+x] <= TILE_EMPTY && !RAND_CHANCE(10))
				Map[y*MAPW+x] =  TILE_EMPTY;

	if (wave == 0)
	{
		for (int i = 0; i != MAPW*MAPH; i++)
			if (i < MAPW || i >= MAPW*MAPH-MAPW || (i%MAPW) == 0 || (i%MAPW) == MAPW-1)
				MapHeights[i] = RAND_RANGE(2.2f, 2.8f);

		for (int y = 1; y != MAPH-1; y++)
			for (int x = 1; x != MAPW-1; x++)
				Map[x+y*MAPW] = TILE_EMPTY;
	}


	if (!wave) return;

	wavespawns = 4+(wave-1)*3;
}


static void Reset()
{
	gameover = 0;
	waveticks = 0;
	wave = 0;
	kills = 0;
	StartWave();

	bullets.clear();
	enemies.clear();
	player = Player();
	player.mtx.SetTranslate(MAPW*.5f+.5f, MAPH*.5f+.5f, 0);
	player.dir = ZLV3(0,1,0);
}

static ZL_Vector AStarMoveTarget(ZL_Vector from, ZL_Vector to)
{
	struct SpiralRange
	{
		inline SpiralRange(int idxstart) : x(idxstart%MAPW), y(idxstart/MAPW) {}
		inline SpiralRange& begin() { return *this; }
		inline SpiralRange& end() { return *this; }
		inline SpiralRange operator++()
		{
			do
			{
				x -= tilex, y -= tiley;
				if ((tilex == tiley) || ((tilex < 0) && (tilex == -tiley)) || ((tilex > 0) && (tilex == 1-tiley))) { int t = deltax; deltax = -deltay; deltay = t; } // Reached a corner, turn left
				x += (tilex += deltax), y += (tiley += deltay);
			} while (x < 1 || x >= MAPW-1 || y < 1 || y >= MAPH-1);
			return *this; 
		}
		inline bool operator!=(const SpiralRange & other) const { return true; }
		inline int operator*() const { return x + y * MAPW; }
		int x, y, tilex = 0, tiley = 0, deltax = 0, deltay = -1;
	};

	int Frontier[MAXMAPSIZE*MAXMAPSIZE], Path[MAXMAPSIZE*MAXMAPSIZE];
	bool Visited[MAXMAPSIZE*MAXMAPSIZE];
	memset(&Visited, 0, sizeof(Visited));
	int ifromx = (int)sfloor(from.x), ifromy = (int)sfloor(from.y);
	int itox = (int)sfloor(to.x), itoy = (int)sfloor(to.y);
	from = ZLV(ZL_Math::Clamp(ifromx, 1, MAPW-1), ZL_Math::Clamp(ifromy, 1, MAPH-1));
	to   = ZLV(ZL_Math::Clamp(itox, 1 + 1, MAPW-1 - 1), ZL_Math::Clamp(itoy, 1 + 1, MAPH-1 - 1));

	int idxFrom = (ifromx + ifromy * MAPW);
	int idxTo   = (itox   + itoy   * MAPW);
	if (idxTo == idxFrom) return to;
	if (Map[idxTo]   == TILE_WALL) { for (int i : SpiralRange(idxTo  )) { if (Map[i] == TILE_EMPTY) { idxTo   = i; break; } } }
	if (Map[idxFrom] == TILE_WALL) { for (int i : SpiralRange(idxFrom)) { if (Map[i] == TILE_EMPTY) { idxFrom = i; break; } } }
	if (idxTo == idxFrom) return to;

	int FrontierDone = 0, FrontierCount = 0;
	Frontier[FrontierCount++] = idxFrom;
	Visited[idxFrom] = true;
	bool InYWall = (idxFrom < MAPW || idxFrom >= MAPW*MAPH-MAPW), InXWall = ((idxFrom%MAPW) == 0 || (idxFrom%MAPW) == MAPW-1);
	while (FrontierDone != FrontierCount)
	{
		int idx = Frontier[FrontierDone++];
		for (int Dir = 0; Dir != 4; Dir++)
		{
			int idxNeighbor;
			switch (Dir)
			{
				case 0: if (InYWall || (idx%MAPW) ==        0) continue; idxNeighbor = idx -    1; break; //left
				case 1: if (InYWall || (idx%MAPW) ==   MAPW-1) continue; idxNeighbor = idx +    1; break; //right
				case 2: if (InXWall ||  idx <            MAPW) continue; idxNeighbor = idx - MAPW; break; //up
				case 3: if (InXWall ||  idx >= MAPW*MAPH-MAPW) continue; idxNeighbor = idx + MAPW; break; //down
			}
			if (Visited[idxNeighbor]) continue;
			Visited[idxNeighbor] = true;
			if (Map[idxNeighbor] != TILE_EMPTY) continue;
			Frontier[FrontierCount++] = idxNeighbor;
			if (idxNeighbor == idxTo)
			{
				int Steps = 0;
				for (int idx1 = idxNeighbor, idx2 = idx, idx3; idx1 != idxFrom; idx1 = idx2, idx2 = idx3, Steps++)
				{
					ZL_ASSERT(idx2 > 0 && idx2 < MAPW*MAPH);
					idx3 = Path[idx2];
					Path[idx2] = idx1;
				}
				int idxTarget = Path[idxFrom]; //[Steps > 1 ? Path[idxFrom] : idxFrom];
				return ZLV((idxTarget%MAPW)+.5f, (idxTarget/MAPW)+.5f);
			}
			Path[idxNeighbor] = idx;
		}
		InYWall = InXWall = false;
	}
	return to; //no path
}

static Thing* DoCollision(Thing& t, float stepHeight)
{
	struct Col { ZL_Vector3 pos, dir; Thing* what; float dist; };
	static std::vector<Col> cols;
	cols.clear();
	ZL_Vector3 tpos = t.mtx.GetTranslate();
	if (tpos.z < -10 || tpos.z > 20)
	{
		// probably a bullet
		return &world;
	}

	int xfrom = ZL_Math::Clamp((int)sfloor(tpos.x - 1.0f), 0, (int)MAPW), xto = ZL_Math::Clamp((int)sfloor(tpos.x + 1.0f), 0, (int)MAPW);
	int yfrom = ZL_Math::Clamp((int)sfloor(tpos.y - 1.0f), 0, (int)MAPH), yto = ZL_Math::Clamp((int)sfloor(tpos.y + 1.0f), 0, (int)MAPH);
	for (int y = yfrom ; y <= yto; y++)
		for (int x = xfrom ; x <= xto; x++)
		{
			int ti = (x + y * MAPW);
			if (Map[ti] == TILE_EMPTY)
			{
				continue;
			}
			if (stepHeight) cols.push_back({ZLV3(x+0.5f, y+0.5f, MapHeights[ti]), ZLV3(0,0,1), &world});
			if (tpos.z < MapHeights[ti] - stepHeight)
			{
				if (tpos.x > s(x + 1)) cols.push_back({ZLV3(x+1.0f, y+0.5f, MapHeights[ti]), ZLV3( 1,0,0), &world});
				if (tpos.x < s(x    )) cols.push_back({ZLV3(x+0.0f, y+0.5f, MapHeights[ti]), ZLV3(-1,0,0), &world});
				if (tpos.y > s(y + 1)) cols.push_back({ZLV3(x+0.5f, y+1.0f, MapHeights[ti]), ZLV3(0, 1,0), &world});
				if (tpos.y < s(y    )) cols.push_back({ZLV3(x+0.5f, y+0.0f, MapHeights[ti]), ZLV3(0,-1,0), &world});
			}
		}

	// ground collision
	cols.push_back({ZLV3(tpos.x, tpos.y, 0), ZLV3(0,0,1), &world});

	if (t.type == Thing::ENEMY_SPIDER)
	{
		for (Enemy& e : enemies)
		{
			if (&e == &t) continue;
			ZL_Vector d = tpos.ToXY() - e.mtx.GetTranslateXY();
			float dist = d.GetLengthSq();
			if (dist > ZL_Math::Square(e.radius + t.radius + .25f)) continue;
			if (dist < 0.01f) continue; // too close to fix
			ZL_Vector dir = d.Norm();
			cols.push_back({e.mtx.GetTranslate() + ZL_Vector3(dir*e.radius, 1.0f), ZL_Vector3(dir), &e});
		}
	}
	if (t.type == Thing::ENEMY_SPIDER) //(&player != &t)
	{
		ZL_Vector d = tpos.ToXY() - player.mtx.GetTranslateXY();
		float dist = d.GetLengthSq();
		if (dist < ZL_Math::Square(player.radius + t.radius + .25f) && dist >= 0.01f)
		{
			ZL_Vector dir = d.Norm();
			cols.push_back({player.mtx.GetTranslate() + ZL_Vector3(dir*player.radius, 1.0f), ZL_Vector3(dir), &player});
		}
	}

	for (Col& c : cols)
		c.dist = tpos.GetDistanceSq(c.pos);

	// because all our collision rects have the same size, we can just sort by closest center
	sort(cols.begin(), cols.end(), [](const Col& a, const Col& b) { return a.dist < b.dist; });

	Thing* collided = NULL;
	float radiusPlusHalf = (t.radius+.5f), radiusPlusHalfSq = (radiusPlusHalf*radiusPlusHalf);
	for (Col& c : cols)
	{
		if (tpos.z >= c.pos.z) continue;
		c.dist = (tpos - c.pos) | c.dir;
		if (c.dist > t.radius) continue;

		// only support straight up or side collision to not have to do full projection
		if (c.dir.z)
		{
			ZL_ASSERT(c.dir.z == 1.0f);
			float x = sabs(tpos.x - c.pos.x);
			if (x > radiusPlusHalf) continue;
			float y = sabs(tpos.y - c.pos.y);
			if (y > radiusPlusHalf) continue;
			tpos.z = c.pos.z;
			if (t.vel.z < 0) t.vel.z = 0;
		}
		else
		{
			ZL_Vector3 pOnPlane = tpos - c.dir * c.dist;
			float f = pOnPlane.ToXY().GetDistanceSq(c.pos.ToXY());
			if (f > radiusPlusHalfSq) continue;
			// push out a tiny bit more to fix warping on edges
			tpos += c.dir * (t.radius - c.dist + 0.001f);
		}
		ZL_ASSERT(c.what);
		collided = c.what;
	}
	if (tpos.x < 0) { tpos.x = 0; collided = &world; }
	if (tpos.y < 0) { tpos.y = 0; collided = &world; }
	if (tpos.x > MAPW) { tpos.x = (float)MAPW; collided = &world; }
	if (tpos.y > MAPH) { tpos.y = (float)MAPH; collided = &world; }
	if (collided) t.mtx.SetTranslate(tpos);
	return collided;
}

static Thing* DoMove(Thing& t, float dt, float stepHeight = 0)
{
	ZL_Vector3 movetotal = t.vel * dt;
	float movelen = movetotal.GetLength();
	Thing* collided = NULL;
	if (movelen > 0)
	{
		ZL_Vector3 movedir = movetotal / movelen;
		for (float step; (step = ZL_Math::Min(movelen, .2f)) > 0; movelen -= step)
		{
			t.mtx.TranslateBy(movedir * step);
			collided = DoCollision(t, stepHeight);
		}
	}
	return collided;
}

static void Load()
{
	fntMain = ZL_Font("Data/typomoderno.ttf.zip", 20.f);
	fntBig = ZL_Font("Data/typomoderno.ttf.zip", 50.f);
	fntTitle = ZL_Font("Data/typomoderno.ttf.zip", 100.f);
	srfCrosshair = ZL_Surface("Data/crosshair.png").SetOrigin(ZL_Origin::Center);

	LightSun.SetColor(ZLRGB(.4,.4,.4));
	LightSun.SetSpotLight(50, 1.0f);

	LightPlayer.SetColor(ZLRGB(.4,.4,.4));
	LightPlayer.SetFalloff(5);

	Camera.SetAmbientLightColor(ZLRGB(.4*.5f,.2,.2));

	using namespace ZL_MaterialModes;

	ZL_Material MatGround = ZL_Material(MM_DIFFUSEMAP).SetDiffuseTexture(ZL_Surface("Data/ground.png").SetTextureRepeatMode());
	MeshGround = ZL_Mesh::BuildPlane(ZLV(MAPW*.5, MAPH*.5), MatGround, ZL_Vector3::Up, ZLV3(MAPW*.5, MAPH*.5, 0), ZLV(MAPW, MAPH));

	ZL_Material MatWall = ZL_Material(MM_DIFFUSEMAP).SetDiffuseTexture(ZL_Surface("Data/wall.png").SetTextureRepeatMode().SetScale(.1f));
	MeshWall = ZL_Mesh::FromPLY("Data/wall.ply", MatWall);
	//MeshWall = ZL_Mesh::BuildBox(ZLV3(.5, .5, 2), MatWall, ZLV3(0,0,-2), ZLV(1,3));

	MeshSpider = ZL_Mesh::BuildPlane(ZLV(.3,.3), ZL_Material(MM_DIFFUSEMAP|MO_MASKED).SetDiffuseTexture(ZL_Surface("Data/spider.png")));
	MeshBat = ZL_Mesh::BuildPlane(ZLV(.3,.3), ZL_Material(MM_DIFFUSEMAP|MO_MASKED).SetDiffuseTexture(ZL_Surface("Data/bat.png")));
	MeshGhost = ZL_Mesh::BuildPlane(ZLV(.5,.5), ZL_Material(MM_DIFFUSEMAP|MO_MASKED).SetDiffuseTexture(ZL_Surface("Data/ghost.png")));

	//MeshBullet = ZL_Mesh::BuildSphere(.1f, 5);
	MeshBullet = ZL_Mesh::BuildPlane(ZLV(.1,.1), ZL_Material(MM_DIFFUSEMAP|MO_UNLIT|MO_MASKED|MO_CASTNOSHADOW).SetDiffuseTexture(ZL_Surface("Data/spark.png")));

	ParticleDamage = ZL_ParticleEmitter(.5f, 500, OP_TRANSPARENT);
	ParticleDamage.SetTexture(ZL_Surface("Data/particle.png"), 1, 1);
	ParticleDamage.SetLifetimeSize(.5f, .05f);
	ParticleDamage.SetSpawnVelocityRanges(ZLV3(-.6,-.6,.1), ZLV3(.6,.6,.6));
	ParticleDamage.SetSpawnColorRange(ZLRGB(.1,.1,.5), ZLRGB(.5,.5,.9));
	ParticleDamage.SetLifetimeAlpha(.3f, 0);

	ParticleDestroy = ZL_ParticleEmitter(1.5f, 500, OP_TRANSPARENT);
	ParticleDestroy.SetTexture(ZL_Surface("Data/particle.png"), 1, 1);
	ParticleDestroy.SetLifetimeSize(.5f, .05f);
	ParticleDestroy.SetSpawnVelocityRanges(ZLV3(-.2,-.2,1), ZLV3(.2,.2,2));
	ParticleDestroy.SetLifetimeAlpha(.3f, 0);

	#ifdef ZILLALOG
	MeshDbgCollision = ZL_Mesh::BuildPlane(ZLV(.3,.3));
	MeshDbgSphere = ZL_Mesh::FromPLY("work/sphere.ply");
	#endif

	extern TImcSongData imcDataIMCMUSIC, imcDataIMCBULLET, imcDataIMCHIT, imcDataIMCHIT2, imcDataIMCJUMP;
	sndBullet = ZL_SynthImcTrack::LoadAsSample(&imcDataIMCBULLET);
	sndHit = ZL_SynthImcTrack::LoadAsSample(&imcDataIMCHIT);
	sndHit2 = ZL_SynthImcTrack::LoadAsSample(&imcDataIMCHIT2);
	sndJump = ZL_SynthImcTrack::LoadAsSample(&imcDataIMCJUMP);
	imcMusic = ZL_SynthImcTrack(&imcDataIMCMUSIC);
	imcMusic.Play();

}

static int CalcAttackCount(float dt, float& timer, float delay, bool attacking)
{
	int n = 0;
	for (float dtx = dt, dtstep; (dtstep = ZL_Math::Min(dtx, delay)) > 0; dtx -= dtstep)
	{
		timer += dtstep;
		if (!attacking) continue;
		if (timer > 0) timer = 0;
		if (timer < 0) continue;
		timer -= delay;
		n++;
	}
	return n;
}

static void Update(float dt)
{
	if (IsTitle) return;
	ZL_Vector md = ZL_Input::MouseDelta();

	if (player.health <= 0) return;

	if (md.x || md.y)
	{
		ZL_Vector3 curdir = player.dir.VecNorm();
		ZL_Vector3 forward = ZL_Vector3(curdir.ToXY().Norm(), 0);
		ZL_Vector3 right = ZL_Vector3(forward.ToXY().RPerp());
		float pitch = curdir.GetRelAbsAngle(forward) * (curdir.z < 0 ? -1 : 1);
		float newpitch = ZL_Math::Clamp(pitch + md.y * SPEED_PITCH, -PIHALF*0.99f, PIHALF*0.99f);
		player.dir.Rotate(right, newpitch - pitch);
		player.dir.Rotate(ZL_Vector3::Up, -md.x * SPEED_YAW);
		player.dir.Norm();
	}

	ZL_Vector wasd = ZLV(((ZL_Input::Held(ZLK_D) || ZL_Input::Held(ZLK_RIGHT)) ? 1.0f : ((ZL_Input::Held(ZLK_A) || ZL_Input::Held(ZLK_LEFT)) ? -1.0f : 0)), 
	                     ((ZL_Input::Held(ZLK_W) || ZL_Input::Held(ZLK_UP   )) ? 1.0f : ((ZL_Input::Held(ZLK_S) || ZL_Input::Held(ZLK_DOWN)) ? -1.0f : 0)));

	bool fire = (!!ZL_Input::Held(ZL_BUTTON_LEFT));
	for (int i = CalcAttackCount(dt, player.weapontimer, WEAPON_DELAY, fire); i--;)
	{
		Bullet b;
		b.mtx = ZL_Matrix::MakeTranslate(player.mtx.GetTranslate() + ZLV3(0, 0, VIEW_HEIGHT*.8f));
		b.vel = player.dir * BULLET_SPEED;
		b.vel.z += 0.1f;
		bullets.push_back(b);
		sndBullet.Play();
	}

	if ((ZL_Input::Down(ZLK_SPACE) || ZL_Input::Down(ZL_BUTTON_RIGHT)) && player.jumps)
	{
		player.jumps--;
		player.vel.z = JUMP_STRENGTH;
		sndJump.Play();
	}
	ZL_Vector forward2d = player.dir.ToXY().Norm();
	ZL_Vector right2d = forward2d.VecRPerp();
	ZL_Vector move = forward2d * (wasd.y * SPEED_FORWARD) + right2d * (wasd.x * SPEED_STRAFE);
	float newvelz = player.vel.z + dt*SPEED_GRAV;
	player.vel = ZL_Vector3::Lerp(player.vel, move, dt*(player.vel.z ? SPEED_AIRACCEL : SPEED_ACCEL));
	player.vel.z = newvelz;

	DoMove(player, dt, CAN_STEP_HEIGHT);
	if (player.vel.z == 0) player.jumps = 2;

	for (size_t i = 0; i != bullets.size(); i++)
	{
		Bullet& b = bullets[i];
		if (DoMove(b, dt))
		{
			bullets.erase(bullets.begin()+(i--));
			continue;
		}

		for (Enemy& e : enemies)
		{
			float distSq = e.mtx.GetTranslate().GetDistanceSq(b.mtx.GetTranslate());
			if (distSq > ZL_Math::Square(e.radius + b.radius)) continue;

			ZL_Vector3 epos = e.mtx.GetTranslate();
			float erad = e.radius * .5f;

			if ((e.health -= 1) <= 0)
			{
				sndHit2.Play();
				for (int pn = 0; pn != 200; pn++)
				{
					ParticleDestroy.SetColor(RAND_COLOR, false);
					ParticleDestroy.Spawn(ZLV3(RAND_RANGE(epos.x-erad, epos.x+erad), RAND_RANGE(epos.y-erad, epos.y+erad), RAND_RANGE(epos.z-erad, epos.z+erad)));
				}
				enemies.erase(enemies.begin() + ((Enemy*)&e - &enemies[0]));
				kills++;
			}
			else
			{
				sndHit.Play();
				for (int pn = 0; pn != 50; pn++)
				{
					ParticleDamage.Spawn(ZLV3(RAND_RANGE(epos.x-erad, epos.x+erad), RAND_RANGE(epos.y-erad, epos.y+erad), RAND_RANGE(epos.z-erad, epos.z+erad)));
				}
				ZL_Vector3 pushback = b.vel.VecNorm() * 0.5f;
				if (pushback.z < 0) pushback.z = 0;
				e.vel += pushback;
				bullets.erase(bullets.begin()+(i--));
			}
			break;
		}
	}

	for (Enemy& e : enemies)
	{
		ZL_Vector3 emove;
		switch (e.type)
		{
			case Thing::ENEMY_SPIDER:
				e.movetarget = AStarMoveTarget(e.mtx.GetTranslateXY(), player.mtx.GetTranslateXY());
				emove = ZL_Vector3((e.movetarget - e.mtx.GetTranslateXY()).Norm(), 0);
				e.vel.z = 0;
				break;
			case Thing::ENEMY_BAT:
			case Thing::ENEMY_GHOST:
			{
				ZL_Vector3 epos = e.mtx.GetTranslate();
				ZL_Vector eposxy = epos.ToXY();
				for (const Enemy& e2 : enemies)
				{
					ZL_Vector3 d = epos - e2.mtx.GetTranslate();
					float distSq = d.GetLengthSq();
					if (distSq < 0.01 || distSq > ZL_Math::Square(e.radius + e2.radius)) continue;
					float back = (e.radius + e2.radius) - ssqrt(distSq);
					e.mtx.TranslateBy(d.VecNorm() * back);
				}
				float targetheight = VIEW_HEIGHT;
				if (epos.z < 2.0f && player.mtx.GetTranslateXY().GetDistance(eposxy) > 5) targetheight = 2.0f;
				emove = ZL_Vector3(player.mtx.GetTranslate() + ZLV3(0, 0, targetheight) - epos).Norm();
				break;
			}
			default:break;
		}
		e.vel = ZL_Vector3::Lerp(e.vel, emove*e.movespeed, dt);
		DoMove(e, dt);

		ZL_Vector3 diff = e.mtx.GetTranslate() - player.mtx.GetTranslate();
		float distSq = diff.GetLengthSq();
		if (distSq < ZL_Math::Square(e.radius + player.radius + .1f) && CalcAttackCount(dt, e.attacktimer, e.attackspeed, true))
		{
			player.lasthit = ZLTICKS;
			player.health -= e.attackdamage;
			if (player.health <= 0)
			{
				ZL_Vector3 ppos = player.mtx.GetTranslate();
				float prad = player.radius * .5f;
				for (int pn = 0; pn != 200; pn++)
				{
					ParticleDestroy.SetColor(RAND_COLOR, false);
					ParticleDestroy.Spawn(ZLV3(RAND_RANGE(ppos.x-prad, ppos.x+prad), RAND_RANGE(ppos.y-prad, ppos.y+prad), RAND_RANGE(ppos.z-prad, ppos.z+prad)));
				}
				bullets.clear();
				gameover = ZLTICKS;
				return;
			}
			ZL_Vector3 pushback = diff.ToXY().Norm();
			player.vel -= pushback * 1.0f;
			e.vel += pushback * 1.0f;
		}
	}
}

static void DrawTextBordered(const ZL_Vector& p, const char* txt, scalar scale = 1, const ZL_Color& colfill = ZLWHITE, const ZL_Color& colborder = ZLBLACK, int border = 2, ZL_Origin::Type origin = ZL_Origin::Center)
{
	for (int i = 0; i < 9; i++) if (i != 4) fntBig.Draw(p.x+(border*((i%3)-1)), p.y+8+(border*((i/3)-1)), txt, scale, scale, colborder, origin);
	fntBig.Draw(p.x  , p.y+8  , txt, scale, scale, colfill, origin);
}

static void Draw()
{
	if (IsTitle)
	{
		float spx = s((ZLTICKS % 600)/3);
		float spr = ssin(ZLTICKS*.03f)*.1f;
		MeshWall.GetMaterial().GetDiffuseTexture().DrawTo(0, 0, ZLWIDTH, ZLHEIGHT);
		ZL_Surface srfSpider = MeshSpider.GetMaterial().GetDiffuseTexture();
		for (int i = 0; i != 10; i++)
		{
			srfSpider.Draw(50+10, -10-200 + spx + i * 200.0f, PI + spr, ZLLUMA(0,.5));
			srfSpider.Draw(50, -200 + spx + i * 200.0f, PI + spr);
			srfSpider.Draw(ZLFROMW(150)+10, -10+ZLHEIGHT + 200 - spx + i * -200.0f, spr, ZLLUMA(0,.5));
			srfSpider.Draw(ZLFROMW(150), ZLHEIGHT + 200 - spx + i * -200.0f, spr);
		}
		ZL_Vector rot = ZL_Vector::FromAngle(ZLTICKS/1000.f) * 20;
		fntTitle.Draw(       ZLHALFW+rot.x,         ZLHALFH + 240-rot.y,         "SHOOTZILLA", 2, 2, ZLLUMA(0,.5), ZL_Origin::Center);
		for (int i = 0; i != 9; i++)
			fntTitle.Draw(   ZLHALFW - 2 + 2*(i%3), ZLHALFH + 240 - 2 + 2*(i/3), "SHOOTZILLA", 2, 2, ZLBLACK, ZL_Origin::Center);
		fntTitle.Draw(       ZLHALFW,               ZLHALFH + 240,               "SHOOTZILLA", 2, 2, ZL_Color::Brown, ZL_Origin::Center);

		DrawTextBordered(ZLV(ZLHALFW, ZLHALFH + 100), "Defat the hordes of evil!");
		DrawTextBordered(ZLV(ZLHALFW, ZLHALFH +  60), "Can you delay the inevitable?");

		DrawTextBordered(ZLV(ZLHALFW, ZLHALFH -  30), "CONTROLS:");
		DrawTextBordered(ZLV(ZLHALFW, ZLHALFH -  75), "MOUSE: Look  /  WASD: Move  /  LEFT CLICK: Attack");
		DrawTextBordered(ZLV(ZLHALFW, ZLHALFH - 120), "SPACE or RIGHT CLICK: (Double)Jump");

		DrawTextBordered(ZLV(ZLHALFW, ZLHALFH - 220), "Press Space to Start!");

		DrawTextBordered(ZLV(ZLHALFW, 30), "(C) 2022 - Bernhard Schelling");

		if (ZL_Input::Down(ZLK_SPACE) || ZL_Input::Down(ZL_BUTTON_LEFT) || ZL_Input::Down(ZL_BUTTON_RIGHT))
		{
			Reset();
			IsTitle = false;
		}
		if (ZL_Input::Down(ZLK_ESCAPE))
		{
			ZL_Application::Quit();
		}
		return;
	}

	if (ZL_Input::Down(ZLK_ESCAPE)) IsTitle = true;
	#ifdef ZILLALOG
	if (ZL_Input::Down(ZLK_F5)) waveticks = ZLTICKS;
	#endif


	ParticleDamage.Update(Camera);
	ParticleDestroy.Update(Camera);

	ZL_Vector3 campos = player.mtx.GetTranslate(), camdir = player.dir;
	campos.z += VIEW_HEIGHT;
	if (gameover)
	{
		float got = ZL_Math::Clamp01(ZLSINCESECONDS(gameover));
		campos.z = ZL_Math::Lerp(campos.z, .1f, got);
		camdir = ZL_Vector3::Lerp(camdir, ZL_Vector3::Up, got).Norm();
	}
	Camera.SetLookAt(campos, campos + camdir);
	ZL_Vector lightang = ZL_Vector::FromAngle(ZLTICKS*.0001f);
	if (lightang.y < 0) { lightang = -lightang; }
	ZL_Vector lightctr = ZLV(MAPW*.5f,MAPH*.5f);
	LightSun.SetLookAt(ZLV3(lightctr.x - MAPW*1.3f * lightang.x, lightctr.y - MAPH*1.3f * lightang.x , 2 + 22 * lightang.y), ZLV3(MAPW*.45f,MAPH*.45f,.1));
	LightPlayer.SetPosition(Camera.GetPosition());

	ZL_Display::FillGradient(0, 0, ZLWIDTH, ZLHEIGHT, ZLRGB(0,0,.3), ZLRGB(0,0,.3), ZLRGB(.4,.4,.4), ZLRGB(.4,.4,.4));
	RenderList.Reset();
	for (Bullet& b : bullets)
	{
		ZL_Vector dXY = (Camera.GetPosition().ToXY() - b.mtx.GetTranslateXY());
		float yaw = dXY.GetAngle() + PIHALF;
		ZL_Vector d2 = ZL_Vector(dXY.GetLength(), Camera.GetPosition().z - b.mtx.GetTranslate().z);
		float pitch = PIHALF+d2.GetRelAngle(ZLV(1,0));
		b.mtx.SetRotate(ZL_Quat::FromRotateZ(yaw) * ZL_Quat::FromRotateX(pitch));

		RenderList.Add(MeshBullet, b.mtx);
	}

	for (Enemy& e : enemies)
	{
		ZL_Vector dXY = (Camera.GetPosition().ToXY() - e.mtx.GetTranslateXY());
		float yaw = dXY.GetAngle() + PIHALF;
		ZL_Vector d2 = ZL_Vector(dXY.GetLength(), Camera.GetPosition().z - e.mtx.GetTranslate().z);
		float pitch = PIHALF+d2.GetRelAngle(ZLV(1,0));

		switch (e.type)
		{
			case Thing::ENEMY_SPIDER:
				e.mtx.SetRotate(ZL_Quat::FromRotateZ(yaw + ssin(ZLTICKS*e.movespeed*.01f)*.1f) * ZL_Quat::FromRotateX(.5f));
				RenderList.Add(MeshSpider, e.mtx);
				break;
			case Thing::ENEMY_BAT:
				e.mtx.SetRotate(ZL_Quat::FromRotateZ(yaw) * ZL_Quat::FromRotateX(pitch + ssin(ZLTICKS*e.movespeed*.01f)*.5f));
				RenderList.Add(MeshBat, e.mtx);
				break;
			case Thing::ENEMY_GHOST:
				e.mtx.SetRotate(ZL_Quat::FromRotateZ(yaw) * ZL_Quat::FromRotateX(pitch));
				RenderList.Add(MeshGhost, e.mtx);
				break;
			default:break;
		}
		#ifdef ZILLALOG
		if (ZL_Input::Held(ZLK_LCTRL)) RenderList.Add(MeshDbgSphere, ZL_Matrix::MakeTranslateScale(e.mtx.GetTranslate(), e.radius));
		#endif
	}

	RenderList.Add(ParticleDamage, ZL_Matrix::Identity);
	RenderList.Add(ParticleDestroy, ZL_Matrix::Identity);

	//ZL_Display3D::DrawListsWithLight(RenderLists, COUNT_OF(RenderLists), Camera, LightSun);
	ZL_Display3D::DrawListsWithLights(RenderLists, COUNT_OF(RenderLists), Camera, Lights, COUNT_OF(Lights));

	if (!gameover)
		srfCrosshair.Draw(ZLHALFW, ZLHALFH-5);

	ZL_Rectf minimap(ZLFROMW(200), ZLFROMH(200), ZLFROMW(20), ZLFROMH(20));
	if (ZL_Input::Held(ZLK_LCTRL)) minimap = ZL_Rectf(ZLFROMW(600), ZLFROMH(600), ZLFROMW(20), ZLFROMH(20));

	ZL_Display::FillRect(minimap, ZL_Color::Black);
	ZL_Display::PushOrtho(0,s(MAPW),0,s(MAPH));
	ZL_Display::Translate(minimap.left * MAPW / ZLWIDTH, minimap.low * MAPH / ZLHEIGHT);
	ZL_Display::Scale(minimap.Width()/ZLWIDTH, minimap.Height()/ZLHEIGHT);
	for (int y = 0; y != MAPH; y++)
		for (int x = 0; x != MAPW; x++)
			if (Map[y*MAPW+x] == '#')
				ZL_Display::FillRect((float)x, (float)y, x+1.f, y+1.f, ZL_Color::Gray);
	ZL_Vector playerpos = player.mtx.GetTranslateXY();
	ZL_Vector playerfwd = player.dir.ToXY().Norm()*.4f, playerside = playerfwd.VecPerp()*.8f;
	ZL_Display::FillTriangle(playerpos-playerside-playerfwd, playerpos+playerside-playerfwd, playerpos+playerfwd, ZLWHITE);
	//ZL_Display::FillCircle(playerpos.x, playerpos.y, player.radius, ZL_Color::White);
	//ZL_Display::FillWideLine(playerpos.ToXY(), playerpos.ToXY() + player.dir.ToXY().Norm(), player.radius*.25f, ZL_Color::White);
	for (Enemy& e : enemies)
	{
		ZL_Display::FillCircle(e.mtx.GetTranslateXY(), .2f, ZL_Color::Red);
		//ZL_Display::FillWideLine(e.mtx.GetTranslateXY(), e.movetarget, .1f, ZL_Color::Red);
	}

	ZL_Display::PopOrtho();

	ZL_Display::DrawRect(0, 0, ZLWIDTH, 30, ZLBLACK, ZLLUMA(1,.5));
	fntMain.Draw(10,10, *ZL_String::format("Wave: %d", wave), ZLBLACK);
	fntMain.Draw(100,10, *ZL_String::format("Enemies: %d", wavespawns + (int)enemies.size()), ZLBLACK);
	fntMain.Draw(210,10,"Health:", ZLBLACK);
	float healthbarx = 280, healthbarwidth = ZLFROMW(10) - healthbarx;
	ZL_Display::FillRect(healthbarx-2, 6, healthbarx+healthbarwidth+2, 24, ZLBLACK);
	if (player.health > 0)
		ZL_Display::FillRect(healthbarx, 8, healthbarx+healthbarwidth*(player.health/player.maxhealth), 22, ZL_Color::Blue);
	float lasthit = ZLSINCE(player.lasthit)*0.01f;
	if (lasthit < 1)
	{
		ZL_Display::FillRect(0, 0, ZLWIDTH, ZLHEIGHT, ZL_Color(1,0,0,.3f-(.3f*lasthit)));
	}

	if (gameover)
	{
		float got = ZLSINCESECONDS(gameover), gotold = ZLSINCESECONDS(gameover+ZLELAPSEDTICKS);
		float t = ZL_Math::Clamp01(got*.5f);
		float x = (t < .5 ? 1.0f-0.5f*ZL_Easing::InOutQuad(t/.5f) : .5f);
		DrawTextBordered(ZLV(ZLWIDTH*x, ZLHALFH) , "Game Over!", 2);
		DrawTextBordered(ZLV(ZLWIDTH*x, ZLHALFH-60), *ZL_String::format("Defeated Enemies: %d", kills));
		DrawTextBordered(ZLV(ZLWIDTH*x, ZLHALFH - 250), "Press Space to return to Title", 1);
		if (got > 1.0)
		{
			if (ZL_Input::Down(ZLK_SPACE) || ZL_Input::Down(ZL_BUTTON_LEFT) || ZL_Input::Down(ZL_BUTTON_RIGHT))
				IsTitle = true;
		}
	}
	else
	{
		if (waveticks == 0) { waveticks = ZLTICKS-2000; }

		float wavet = ZLSINCESECONDS(waveticks), wavetold = ZLSINCESECONDS(waveticks+ZLELAPSEDTICKS);
		if (wavet >= 0 && wavetold < 2)
		{
			float t = ZL_Math::Clamp01(wavet*.5f);
			float x = (t < .3 ? 1.0f-0.5f*ZL_Easing::InOutQuad(t/.3f) : (t < .6f ? 0.5f : 0.5f-ZL_Easing::InOutQuad((t-.6f)/.3f)));
			DrawTextBordered(ZLV(ZLWIDTH*x, ZLHALFH) , "You Win!", 2);
			FadeWalls(1-t);
		}
		if (wavetold < 2.0f && wavet >= 2.0f)
		{
			wave++;
			StartWave();
		}
		if (wavet >= 2 && wavetold < 4)
		{
			float t = ZL_Math::Clamp01((wavet-2)*.5f);
			float x = (t < .3 ? 1.0f-0.5f*ZL_Easing::InOutQuad(t/.3f) : (t < .6f ? 0.5f : 0.5f-ZL_Easing::InOutQuad((t-.6f)/.3f)));
			DrawTextBordered(ZLV(ZLWIDTH*x, ZLHALFH+55), *ZL_String::format("Wave: %d", wave), 2);
			DrawTextBordered(ZLV(ZLWIDTH*x, ZLHALFH-60), *ZL_String::format("Enemies: %d", wavespawns + (int)enemies.size()), 1);
			FadeWalls(t);
		}
		float spawnspeed = 1.0f + wave / 30.0f;
		if (wavet >= 5 && wavespawns && (int)(wavetold*spawnspeed) != (int)(wavet*spawnspeed))
		{
			wavespawns--;
			SpawnEnemy();
		}
		if (wavet >= 5 && !wavespawns && !enemies.size())
		{
			waveticks = ZLTICKS;
		}
	}
}

static struct sShootzilla : public ZL_Application
{
	sShootzilla() : ZL_Application(60) { }

	virtual void Load(int argc, char *argv[])
	{
		if (!ZL_Application::LoadReleaseDesktopDataBundle()) return;
		if (!ZL_Display::Init("Shootzilla", 1280, 720, ZL_DISPLAY_ALLOWRESIZEHORIZONTAL | ZL_DISPLAY_DEPTHBUFFER)) return;
		ZL_Display::ClearFill(ZL_Color::White);
		ZL_Display::SetAA(true);
		ZL_Display3D::Init(2);
		ZL_Display3D::InitShadowMapping();
		ZL_Audio::Init();
		ZL_Input::Init();
		ZL_Display::SetPointerLock(true);
		::Load();
		::Reset();
	}

	virtual void AfterFrame()
	{
		::Update(ZL_Math::Min(ZLELAPSED, .333f));
		::Draw();
	}
} Shootzilla;

#if 1 // MUSIC/SOUND DATA
static const unsigned int IMCMUSIC_OrderTable[] = {
	0x000000003, 0x000000001, 0x000000001, 0x000000001, 0x000000001, 0x000000002, 0x000000002,
};
static const unsigned char IMCMUSIC_PatternData[] = {
	0x1B, 0, 0x19, 0, 0x20, 0, 0x19, 0, 0x1B, 0, 0x19, 0, 0x20, 0, 0x19, 0,
	0x20, 0, 0x20, 0, 0x20, 0, 0x22, 0, 0x22, 0, 0x22, 0, 0x24, 0, 0, 0,
	0x20, 0x20, 0x1B, 0, 0, 0, 0, 0, 0x20, 0x20, 0x1B, 0, 0, 0, 0, 0,
};
static const unsigned char IMCMUSIC_PatternLookupTable[] = { 0, 3, 3, 3, 3, 3, 3, 3, };
static const TImcSongEnvelope IMCMUSIC_EnvList[] = {
	{ 0, 256, 261, 25, 31, 255, true, 255, },
	{ 0, 256, 152, 8, 16, 255, true, 255, },
	{ 0, 256, 173, 8, 16, 255, true, 255, },
};
static TImcSongEnvelopeCounter IMCMUSIC_EnvCounterList[] = {
	{ 0, 0, 2 }, { -1, -1, 256 }, { 1, 0, 256 }, { 2, 0, 256 },
	{ 2, 0, 256 },
};
static const TImcSongOscillator IMCMUSIC_OscillatorList[] = {
	{ 8, 0, IMCSONGOSCTYPE_SINE, 0, -1, 100, 1, 1 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 0, -1, 66, 1, 1 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 0, -1, 24, 1, 1 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 0, -1, 88, 2, 1 },
	{ 10, 0, IMCSONGOSCTYPE_SQUARE, 0, -1, 62, 3, 1 },
	{ 9, 0, IMCSONGOSCTYPE_SQUARE, 0, -1, 34, 4, 1 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 0, 1, 36, 1, 1 },
	{ 8, 0, IMCSONGOSCTYPE_NOISE, 0, 3, 14, 1, 1 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 1, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 2, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 3, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 4, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 5, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 6, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 7, -1, 100, 0, 0 },
};
static const TImcSongEffect IMCMUSIC_EffectList[] = {
	{ 226, 173, 1, 0, IMCSONGEFFECTTYPE_RESONANCE, 1, 1 },
	{ 204, 0, 1, 0, IMCSONGEFFECTTYPE_LOWPASS, 1, 0 },
	{ 10795, 655, 1, 0, IMCSONGEFFECTTYPE_OVERDRIVE, 0, 1 },
};
static unsigned char IMCMUSIC_ChannelVol[8] = { 97, 100, 100, 100, 100, 100, 100, 100 };
static const unsigned char IMCMUSIC_ChannelEnvCounter[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
static const bool IMCMUSIC_ChannelStopNote[8] = { true, false, false, false, false, false, false, false };
TImcSongData imcDataIMCMUSIC = {
	/*LEN*/ 0x7, /*ROWLENSAMPLES*/ 5512, /*ENVLISTSIZE*/ 3, /*ENVCOUNTERLISTSIZE*/ 5, /*OSCLISTSIZE*/ 15, /*EFFECTLISTSIZE*/ 3, /*VOL*/ 80,
	IMCMUSIC_OrderTable, IMCMUSIC_PatternData, IMCMUSIC_PatternLookupTable, IMCMUSIC_EnvList, IMCMUSIC_EnvCounterList, IMCMUSIC_OscillatorList, IMCMUSIC_EffectList,
	IMCMUSIC_ChannelVol, IMCMUSIC_ChannelEnvCounter, IMCMUSIC_ChannelStopNote };

static const unsigned int IMCBULLET_OrderTable[] = {
	0x000000001,
};
static const unsigned char IMCBULLET_PatternData[] = {
	0x42, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const unsigned char IMCBULLET_PatternLookupTable[] = { 0, 1, 1, 1, 1, 1, 1, 1, };
static const TImcSongEnvelope IMCBULLET_EnvList[] = {
	{ 0, 256, 87, 8, 16, 255, true, 255, },
	{ 0, 256, 5, 8, 16, 255, false, 255, },
};
static TImcSongEnvelopeCounter IMCBULLET_EnvCounterList[] = {
	{ 0, 0, 256 }, { -1, -1, 256 }, { 1, 0, 256 },
};
static const TImcSongOscillator IMCBULLET_OscillatorList[] = {
	{ 8, 0, IMCSONGOSCTYPE_SAW, 0, -1, 100, 1, 1 },
	{ 9, 0, IMCSONGOSCTYPE_SINE, 0, 0, 100, 1, 1 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 1, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 2, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 3, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 4, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 5, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 6, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 7, -1, 100, 0, 0 },
};
static const TImcSongEffect IMCBULLET_EffectList[] = {
	{ 255, 0, 1, 0, IMCSONGEFFECTTYPE_HIGHPASS, 2, 0 },
	{ 128, 0, 2594, 0, IMCSONGEFFECTTYPE_DELAY, 0, 0 },
};
static unsigned char IMCBULLET_ChannelVol[8] = { 100, 100, 100, 100, 100, 100, 100, 100 };
static const unsigned char IMCBULLET_ChannelEnvCounter[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
static const bool IMCBULLET_ChannelStopNote[8] = { true, false, false, false, false, false, false, false };
TImcSongData imcDataIMCBULLET = {
	/*LEN*/ 0x1, /*ROWLENSAMPLES*/ 2594, /*ENVLISTSIZE*/ 2, /*ENVCOUNTERLISTSIZE*/ 3, /*OSCLISTSIZE*/ 9, /*EFFECTLISTSIZE*/ 2, /*VOL*/ 100,
	IMCBULLET_OrderTable, IMCBULLET_PatternData, IMCBULLET_PatternLookupTable, IMCBULLET_EnvList, IMCBULLET_EnvCounterList, IMCBULLET_OscillatorList, IMCBULLET_EffectList,
	IMCBULLET_ChannelVol, IMCBULLET_ChannelEnvCounter, IMCBULLET_ChannelStopNote };

static const unsigned int IMCHIT_OrderTable[] = {
	0x000000001,
};
static const unsigned char IMCHIT_PatternData[] = {
	0x5C, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const unsigned char IMCHIT_PatternLookupTable[] = { 0, 1, 1, 1, 1, 1, 1, 1, };
static const TImcSongEnvelope IMCHIT_EnvList[] = {
	{ 0, 256, 244, 0, 24, 255, true, 255, },
	{ 0, 256, 244, 0, 255, 255, true, 255, },
	{ 100, 200, 30, 5, 255, 255, true, 255, },
	{ 0, 256, 38, 0, 24, 255, true, 255, },
	{ 0, 256, 25, 2, 255, 255, true, 255, },
	{ 0, 256, 38, 11, 255, 255, true, 255, },
};
static TImcSongEnvelopeCounter IMCHIT_EnvCounterList[] = {
	{ 0, 0, 128 }, { 1, 0, 128 }, { -1, -1, 72 }, { 2, 0, 192 },
	{ -1, -1, 256 }, { 3, 0, 128 }, { 4, 0, 184 }, { 5, 0, 238 },
};
static const TImcSongOscillator IMCHIT_OscillatorList[] = {
	{ 7, 221, IMCSONGOSCTYPE_SINE, 0, -1, 132, 1, 2 },
	{ 8, 200, IMCSONGOSCTYPE_SINE, 0, -1, 68, 5, 4 },
	{ 8, 0, IMCSONGOSCTYPE_NOISE, 0, 0, 150, 3, 4 },
	{ 8, 0, IMCSONGOSCTYPE_NOISE, 0, 1, 254, 4, 4 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 1, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 2, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 3, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 4, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 5, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 6, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 7, -1, 100, 0, 0 },
};
static const TImcSongEffect IMCHIT_EffectList[] = {
	{ 86, 197, 1, 0, IMCSONGEFFECTTYPE_RESONANCE, 6, 7 },
	{ 99, 0, 1, 0, IMCSONGEFFECTTYPE_LOWPASS, 4, 0 },
};
static unsigned char IMCHIT_ChannelVol[8] = { 97, 100, 100, 100, 100, 100, 100, 100 };
static const unsigned char IMCHIT_ChannelEnvCounter[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
static const bool IMCHIT_ChannelStopNote[8] = { true, false, false, false, false, false, false, false };
TImcSongData imcDataIMCHIT = {
	/*LEN*/ 0x1, /*ROWLENSAMPLES*/ 2594, /*ENVLISTSIZE*/ 6, /*ENVCOUNTERLISTSIZE*/ 8, /*OSCLISTSIZE*/ 11, /*EFFECTLISTSIZE*/ 2, /*VOL*/ 150,
	IMCHIT_OrderTable, IMCHIT_PatternData, IMCHIT_PatternLookupTable, IMCHIT_EnvList, IMCHIT_EnvCounterList, IMCHIT_OscillatorList, IMCHIT_EffectList,
	IMCHIT_ChannelVol, IMCHIT_ChannelEnvCounter, IMCHIT_ChannelStopNote };


static const unsigned char IMCHIT2_PatternData[] = {
	0x50, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
TImcSongData imcDataIMCHIT2 = {
	/*LEN*/ 0x1, /*ROWLENSAMPLES*/ 2594, /*ENVLISTSIZE*/ 6, /*ENVCOUNTERLISTSIZE*/ 8, /*OSCLISTSIZE*/ 11, /*EFFECTLISTSIZE*/ 2, /*VOL*/ 150,
	IMCHIT_OrderTable, IMCHIT2_PatternData, IMCHIT_PatternLookupTable, IMCHIT_EnvList, IMCHIT_EnvCounterList, IMCHIT_OscillatorList, IMCHIT_EffectList,
	IMCHIT_ChannelVol, IMCHIT_ChannelEnvCounter, IMCHIT_ChannelStopNote };

static const unsigned int IMCJUMP_OrderTable[] = {
	0x000000001,
};
static const unsigned char IMCJUMP_PatternData[] = {
	0x40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const unsigned char IMCJUMP_PatternLookupTable[] = { 0, 1, 1, 1, 1, 1, 1, 1, };
static const TImcSongEnvelope IMCJUMP_EnvList[] = {
	{ 0, 256, 64, 8, 16, 255, true, 255, },
	{ 0, 256, 2092, 24, 16, 16, true, 255, },
	{ 0, 256, 64, 27, 255, 255, true, 255, },
	{ 0, 256, 697, 8, 16, 255, true, 255, },
	{ 0, 256, 1046, 8, 16, 255, true, 255, },
	{ 200, 300, 15, 8, 255, 255, false, 255, },
};
static TImcSongEnvelopeCounter IMCJUMP_EnvCounterList[] = {
	{ 0, 0, 256 }, { -1, -1, 256 }, { 1, 0, 0 }, { 2, 0, 18 },
	{ -1, -1, 128 }, { 3, 0, 256 }, { 4, 0, 256 }, { 5, 0, 300 },
};
static const TImcSongOscillator IMCJUMP_OscillatorList[] = {
	{ 8, 0, IMCSONGOSCTYPE_SAW, 0, -1, 0, 1, 1 },
	{ 8, 1, IMCSONGOSCTYPE_SAW, 0, -1, 0, 1, 1 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 0, -1, 255, 2, 3 },
	{ 9, 1, IMCSONGOSCTYPE_SAW, 0, -1, 0, 1, 1 },
	{ 7, 0, IMCSONGOSCTYPE_SINE, 0, -1, 86, 5, 6 },
	{ 4, 48, IMCSONGOSCTYPE_NOISE, 0, 2, 10, 1, 4 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 1, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 2, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 3, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 4, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 5, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 6, -1, 100, 0, 0 },
	{ 8, 0, IMCSONGOSCTYPE_SINE, 7, -1, 100, 0, 0 },
};
static const TImcSongEffect IMCJUMP_EffectList[] = {
	{ 14859, 562, 1, 0, IMCSONGEFFECTTYPE_OVERDRIVE, 0, 1 },
	{ 255, 121, 1, 0, IMCSONGEFFECTTYPE_RESONANCE, 1, 1 },
	{ 0, 0, 301, 0, IMCSONGEFFECTTYPE_FLANGE, 7, 0 },
};
static unsigned char IMCJUMP_ChannelVol[8] = { 104, 100, 100, 100, 100, 100, 100, 100 };
static const unsigned char IMCJUMP_ChannelEnvCounter[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
static const bool IMCJUMP_ChannelStopNote[8] = { true, false, false, false, false, false, false, false };
TImcSongData imcDataIMCJUMP = {
	/*LEN*/ 0x1, /*ROWLENSAMPLES*/ 2594, /*ENVLISTSIZE*/ 6, /*ENVCOUNTERLISTSIZE*/ 8, /*OSCLISTSIZE*/ 13, /*EFFECTLISTSIZE*/ 3, /*VOL*/ 45,
	IMCJUMP_OrderTable, IMCJUMP_PatternData, IMCJUMP_PatternLookupTable, IMCJUMP_EnvList, IMCJUMP_EnvCounterList, IMCJUMP_OscillatorList, IMCJUMP_EffectList,
	IMCJUMP_ChannelVol, IMCJUMP_ChannelEnvCounter, IMCJUMP_ChannelStopNote };
#endif
