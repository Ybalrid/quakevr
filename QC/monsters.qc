/* ALL MONSTERS SHOULD BE 1 0 0 IN COLOR */

// name =[framenum,    nexttime, nextthink] {code}
// expands to:
// name ()
// {
//        self.frame=framenum;
//        self.nextthink = time + nexttime;
//        self.think = nextthink
//        <code>
// };


/*
================
monster_use

Using a monster makes it angry at the current activator
================
*/
void() monster_use =
{
    if(self.enemy)
        return;

    if(self.health <= 0)
        return;

    if(VR_ItemUtil_EntHasItem(activator, IID_INVISIBILITY))
        return;

    if(activator.flags & FL_NOTARGET)
        return;

    if(activator.classname != "player")
        return;

// delay reaction so if the monster is teleported, its sound is still
// heard
    self.enemy = activator;
    self.nextthink = time + 0.1;
    self.think = FoundTarget;
};

/*
================
monster_death_use

When a monster dies, it fires all of its targets with the current
enemy as activator.
================
*/
void() monster_death_use =
{
    // fall to ground
    if(self.flags & FL_FLY)
        self.flags = self.flags - FL_FLY;
    if(self.flags & FL_SWIM)
        self.flags = self.flags - FL_SWIM;

    if(!self.target && /* HONEY */ !self.killtarget)
        return;

    activator = self.enemy;
    SUB_UseTargets();
};


//============================================================================

void() walkmonster_start_go =
{
    self.origin_z = self.origin_z + 1;    // raise off floor a bit

    // HONEY
    if(self.classname == "monster_tarbaby" &&
       self.spawnflags & SPAWNFLAG_TBABY_HIPHOP)
    {
        // Don't do anything..
    }
    else
    {
        droptofloor();
    }


    self.takedamage = DAMAGE_AIM;

    self.ideal_yaw = self.angles * '0 1 0';
    if(!self.yaw_speed)
        self.yaw_speed = 20;
    self.view_ofs = '0 0 25';
    self.use = monster_use;

    self.flags = self.flags | FL_MONSTER;

    if(self.target != NULLSTR)
    {
        self.goalentity = self.movetarget = find(world, targetname, self.target);
        self.ideal_yaw = vectoyaw(self.goalentity.origin - self.origin);
        if(!self.movetarget)
        {
            dprint("Monster can't find target at ");
            dprint(vtos(self.origin));
            dprint("\n");
        }
// this used to be an objerror
        if(self.movetarget.classname == "path_corner")
            self.th_walk();
        else
            self.pausetime = 99999999;
            self.th_stand();
    }
    else
    {
        self.pausetime = 99999999;
        self.th_stand();
    }

    // HONEY
    if(self.spawnflags & SPAWNFLAG_TRIGGER_SPAWN_ANGRY){
        if(activator.items & IT_INVISIBILITY)
            return;
        if(activator.flags & FL_NOTARGET)
            return;
        self.enemy = activator;
        self.nextthink = time + 0.1;
        self.think = FoundTarget;
    }else{
        self.enemy = world;
    }

// spread think times so they don't all happen at same time
    self.nextthink = self.nextthink + random()*0.5;
};


void() walkmonster_start =
{
// delay drop to floor to make sure all doors have been spawned
// spread think times so they don't all happen at same time
    self.nextthink = self.nextthink + random()*0.5;
    self.flags = self.flags | FL_MONSTER;
    self.think = walkmonster_start_go;

    // total_monsters = total_monsters + 1;

    // HONEY
    if(self.spawnflags & SPAWNFLAG_TRIGGER_SPAWN && !intermission_running)
    {
        WriteByte(MSG_ALL, SVC_UPDATESTAT);
        WriteByte(MSG_ALL, 12);
        WriteLong(MSG_ALL, total_monsters);
    }
};



void() flymonster_start_go =
{
    self.takedamage = DAMAGE_AIM;

    self.ideal_yaw = self.angles * '0 1 0';
    if(!self.yaw_speed)
        self.yaw_speed = 10;
    self.view_ofs = '0 0 25';
    self.use = monster_use;

    self.flags = self.flags | FL_FLY;
    self.flags = self.flags | FL_MONSTER;

    if(!walkmove(0,0))
    {
        dprint("flymonster in wall!\n");
        dprint(self.classname);
        dprint(" at ");
        dprint(vtos(self.origin));
        dprint("\n");
    }

    if(self.target != NULLSTR)
    {
        self.goalentity = self.movetarget = find(world, targetname, self.target);
        if(!self.movetarget)
        {
            dprint("Monster can't find target at ");
            dprint(vtos(self.origin));
            dprint("\n");
        }
// this used to be an objerror
        if(self.movetarget.classname == "path_corner")
            self.th_walk();
        else
            self.pausetime = 99999999;
            self.th_stand();
    }
    else
    {
        self.pausetime = 99999999;
        self.th_stand();
    }

    // HONEY
    if(self.spawnflags & SPAWNFLAG_TRIGGER_SPAWN_ANGRY){
        if(activator.items & IT_INVISIBILITY)
            return;
        if(activator.flags & FL_NOTARGET)
            return;

        //self.enemy = activator;
        self.nextthink = time + 0.1;
        self.think = FoundTarget;
    }else{
        self.enemy = world;
    }
};

void() flymonster_start =
{
    self.enemy = activator;

// spread think times so they don't all happen at same time
    self.nextthink = self.nextthink + random()*0.5;
    self.flags = self.flags | FL_MONSTER;
    self.think = flymonster_start_go;

    // total_monsters = total_monsters + 1;

    // HONEY
    if(self.spawnflags & SPAWNFLAG_TRIGGER_SPAWN && !intermission_running)
    {
        WriteByte(MSG_ALL, SVC_UPDATESTAT);
        WriteByte(MSG_ALL, 12);
        WriteLong (MSG_ALL, total_monsters);
    }
};


void() swimmonster_start_go =
{
    if(deathmatch)
    {
        remove(self);
        return;
    }

    self.takedamage = DAMAGE_AIM;

    self.ideal_yaw = self.angles * '0 1 0';
    if(!self.yaw_speed)
        self.yaw_speed = 10;
    self.view_ofs = '0 0 10';
    self.use = monster_use;

    self.flags = self.flags | FL_SWIM;
    self.flags = self.flags | FL_MONSTER;

    if(self.target != NULLSTR)
    {
        self.goalentity = self.movetarget = find(world, targetname, self.target);
        if(!self.movetarget)
        {
            dprint("Monster can't find target at ");
            dprint(vtos(self.origin));
            dprint("\n");
        }
// this used to be an objerror
        self.ideal_yaw = vectoyaw(self.goalentity.origin - self.origin);
        self.th_walk();
    }
    else
    {
        self.pausetime = 99999999;
        self.th_stand();
    }

    // HONEY
    if(self.spawnflags & SPAWNFLAG_TRIGGER_SPAWN_ANGRY){
        if(activator.items & IT_INVISIBILITY)
            return;
        if(activator.flags & FL_NOTARGET)
            return;

        //self.enemy = activator;
        self.nextthink = time + 0.1;
        self.think = FoundTarget;
    }else{
        self.enemy = world;
    }

    // spread think times so they don't all happen at same time
    self.nextthink = self.nextthink + random()*0.5;
};

void() swimmonster_start =
{
    self.enemy = activator;

    // spread think times so they don't all happen at same time
    self.flags = self.flags | FL_MONSTER;
    self.nextthink = self.nextthink + random()*0.5;
    self.think = swimmonster_start_go;

    // total_monsters = total_monsters + 1;

    if(self.spawnflags & SPAWNFLAG_TRIGGER_SPAWN && !intermission_running)
    {
        WriteByte(MSG_ALL, SVC_UPDATESTAT);
        WriteByte(MSG_ALL, 12);
        WriteLong (MSG_ALL, total_monsters);
    }
};
