#ifndef BLENDER_H
#define BLENDER_H

// Generated using BLENDER-v405
#define BLENDER_VERSION 405

#include <inttypes.h>	 // int64_t uint64_t

// This has some effect on _MSC_VER only AFAIK
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif
#ifdef rad2
#undef rad2
#endif

#ifdef __cplusplus
#	ifndef uchar
	typedef unsigned char uchar;
#	endif
#	ifndef ushort
	typedef unsigned short ushort;
#	endif
namespace Blender	{
extern "C"	{
#endif // __cplusplus


struct raw_data	{
};

struct IDPropertyUIData	{
	char	*description;
	int	rna_subtype;
	char	_pad[4];
};

struct IDPropertyUIDataEnumItem	{
	char	*identifier;
	char	*name;
	char	*description;
	int	value;
	int	icon;
};

struct IDPropertyUIDataInt	{
	IDPropertyUIData	base;
	int	*default_array;
	int	default_array_len;
	int	min;
	int	max;
	int	soft_min;
	int	soft_max;
	int	step;
	int	default_value;
	int	enum_items_num;
	IDPropertyUIDataEnumItem	*enum_items;
};

struct IDPropertyUIDataBool	{
	IDPropertyUIData	base;
	int8_t	*default_array;
	int	default_array_len;
	char	_pad[3];
	int8_t	default_value;
};

struct IDPropertyUIDataFloat	{
	IDPropertyUIData	base;
	double	*default_array;
	int	default_array_len;
	char	_pad[4];
	float	step;
	int	precision;
	double	min;
	double	max;
	double	soft_min;
	double	soft_max;
	double	default_value;
};

struct IDPropertyUIDataString	{
	IDPropertyUIData	base;
	char	*default_value;
};

struct IDPropertyUIDataID	{
	IDPropertyUIData	base;
	short	id_type;
	char	_pad[6];
};

struct ListBase	{
	void	*first;
	void	*last;
};

struct IDPropertyData	{
	void	*pointer;
	ListBase	group;
	int	val;
	int	val2;
};

struct IDProperty	{
	IDProperty	*next;
	IDProperty	*prev;
	char	type;
	char	subtype;
	short	flag;
	char	name[64];
	char	_pad0[4];
	IDPropertyData	data;
	int	len;
	int	totallen;
	IDPropertyUIData	*ui_data;
};

struct ID;
struct IDOverrideLibraryPropertyOperation	{
	IDOverrideLibraryPropertyOperation	*next;
	IDOverrideLibraryPropertyOperation	*prev;
	short	operation;
	short	flag;
	short	tag;
	char	_pad0[2];
	char	*subitem_reference_name;
	char	*subitem_local_name;
	int	subitem_reference_index;
	int	subitem_local_index;
	ID	*subitem_reference_id;
	ID	*subitem_local_id;
};

struct IDOverrideLibraryProperty	{
	IDOverrideLibraryProperty	*next;
	IDOverrideLibraryProperty	*prev;
	char	*rna_path;
	ListBase	operations;
	short	tag;
	char	_pad[2];
	int	rna_prop_type;
};

struct IDOverrideLibrary	{
	ID	*reference;
	ListBase	properties;
	ID	*hierarchy_root;
	void	*runtime;
	int	flag;
	char	_pad_1[4];
};

struct ID_Runtime_Remap	{
	int	status;
	int	skipped_refcounted;
	int	skipped_direct;
	int	skipped_indirect;
};

struct ID_Runtime	{
	ID_Runtime_Remap	remap;
	void	*depsgraph;
	void	*readfile_data;
};

struct Library;
struct AssetMetaData;
struct LibraryWeakReference;
struct ID	{
	void	*next;
	void	*prev;
	ID	*newid;
	Library	*lib;
	AssetMetaData	*asset_data;
	char	name[66];
	short	flag;
	int	tag;
	int	us;
	int	icon_id;
	int	recalc;
	int	recalc_up_to_undo_push;
	int	recalc_after_undo_push;
	int	session_uid;
	IDProperty	*properties;
	IDProperty	*system_properties;
	void	*_pad1;
	IDOverrideLibrary	*override_library;
	ID	*orig_id;
	void	*py_instance;
	LibraryWeakReference	*library_weak_reference;
	ID_Runtime	runtime;
};

struct PackedFile;
struct Library	{
	ID	id;
	char	name[1024];
	PackedFile	*packedfile;
	void	*runtime;
};

struct LibraryWeakReference	{
	char	library_filepath[1024];
	char	library_id_name[66];
	char	_pad[2];
};

struct PreviewImage	{
	int	w[2];
	int	h[2];
	short	flag[2];
	short	changed_timestamp[2];
	int	*rect[2];
	void	*runtime;
};

struct bMotionPathVert	{
	float	co[3];
	int	flag;
};

struct bMotionPath	{
	bMotionPathVert	*points;
	int	length;
	int	start_frame;
	int	end_frame;
	float	color[3];
	float	color_post[3];
	int	line_thickness;
	int	flag;
	char	_pad2[4];
	void	*points_vbo;
	void	*batch_line;
	void	*batch_points;
	void	*_pad;
};

struct bAnimVizSettings	{
	short	recalc;
	short	path_type;
	short	path_step;
	short	path_range;
	short	path_viewflag;
	short	path_bakeflag;
	char	_pad[4];
	int	path_sf;
	int	path_ef;
	int	path_bc;
	int	path_ac;
};

struct bPoseChannel_BBoneSegmentBoundary	{
	float	point[3];
	float	plane_normal[3];
	float	plane_offset;
	float	depth_scale;
};

struct SessionUID	{
	uint64_t	uid_;
};

struct DualQuat	{
	float	quat[4];
	float	trans[4];
	float	scale[4][4];
	float	scale_weight;
};

struct bPoseChannel_Runtime	{
	SessionUID	session_uid;
	DualQuat	deform_dual_quat;
	int	bbone_segments;
	float	bbone_arc_length_reciprocal;
	char	_pad1[4];
	void	*bbone_rest_mats;
	void	*bbone_pose_mats;
	void	*bbone_deform_mats;
	DualQuat	*bbone_dual_quats;
	bPoseChannel_BBoneSegmentBoundary	*bbone_segment_boundaries;
	void	*_pad;
};

struct Bone;
struct Object;
struct ThemeWireColor	{
	char	solid[4];
	char	select[4];
	char	active[4];
	short	flag;
	char	_pad0[2];
};

struct BoneColor	{
	int8_t	palette_index;
	uchar	_pad0[7];
	ThemeWireColor	custom;
};

struct bPoseChannel	{
	bPoseChannel	*next;
	bPoseChannel	*prev;
	IDProperty	*prop;
	IDProperty	*system_properties;
	ListBase	constraints;
	char	name[64];
	short	flag;
	short	ikflag;
	short	protectflag;
	short	agrp_index;
	char	constflag;
	char	selectflag;
	char	drawflag;
	char	bboneflag;
	char	_pad0[4];
	Bone	*bone;
	bPoseChannel	*parent;
	bPoseChannel	*child;
	ListBase	iktree;
	ListBase	siktree;
	bMotionPath	*mpath;
	Object	*custom;
	bPoseChannel	*custom_tx;
	float	custom_scale;
	float	custom_scale_xyz[3];
	float	custom_translation[3];
	float	custom_rotation_euler[3];
	float	custom_shape_wire_width;
	float	loc[3];
	float	size[3];
	float	eul[3];
	float	quat[4];
	float	rotAxis[3];
	float	rotAngle;
	short	rotmode;
	char	_pad[6];
	float	chan_mat[4][4];
	float	pose_mat[4][4];
	float	disp_mat[4][4];
	float	disp_tail_mat[4][4];
	float	constinv[4][4];
	float	pose_head[3];
	float	pose_tail[3];
	float	limitmin[3];
	float	limitmax[3];
	float	stiffness[3];
	float	ikstretch;
	float	ikrotweight;
	float	iklinweight;
	float	roll1;
	float	roll2;
	float	curveInX;
	float	curveInY;
	float	curveOutX;
	float	curveOutY;
	float	ease1;
	float	ease2;
	float	scaleIn;
	float	scale_in_y;
	float	scaleOut;
	float	scale_out_y;
	float	scale_in[3];
	float	scale_out[3];
	bPoseChannel	*bbone_prev;
	bPoseChannel	*bbone_next;
	void	*temp;
	void	*draw_data;
	bPoseChannel	*orig_pchan;
	BoneColor	color;
	void	*_pad2;
	bPoseChannel_Runtime	runtime;
};

struct bPose	{
	ListBase	chanbase;
	void	*chanhash;
	bPoseChannel	**chan_array;
	short	flag;
	char	_pad[2];
	float	ctime;
	float	stride_offset[3];
	float	cyclic_offset[3];
	ListBase	agroups;
	int	active_group;
	int	iksolver;
	void	*ikdata;
	void	*ikparam;
	bAnimVizSettings	avs;
};

struct bIKParam	{
	int	iksolver;
};

struct bItasc	{
	int	iksolver;
	float	precision;
	short	numiter;
	short	numstep;
	float	minstep;
	float	maxstep;
	short	solver;
	short	flag;
	float	feedback;
	float	maxvel;
	float	dampmax;
	float	dampeps;
};

struct ActionChannelBag;
struct bActionGroup	{
	bActionGroup	*next;
	bActionGroup	*prev;
	ListBase	channels;
	int	fcurve_range_start;
	int	fcurve_range_length;
	ActionChannelBag	*channel_bag;
	int	flag;
	int	customCol;
	char	name[64];
	ThemeWireColor	cs;
};

struct ActionLayer;
struct ActionSlot;
struct ActionStripKeyframeData;
struct bAction	{
	ID	id;
	ActionLayer	**layer_array;
	int	layer_array_num;
	int	layer_active_index;
	ActionSlot	**slot_array;
	int	slot_array_num;
	int	last_slot_handle;
	ActionStripKeyframeData	**strip_keyframe_data_array;
	int	strip_keyframe_data_array_num;
	char	_pad0[4];
	ListBase	curves;
	ListBase	chanbase;
	ListBase	groups;
	ListBase	markers;
	int	flag;
	int	active_marker;
	int	idroot;
	char	_pad1[4];
	float	frame_start;
	float	frame_end;
	PreviewImage	*preview;
};

struct Collection;
struct bDopeSheet	{
	ID	*source;
	ListBase	chanbase;
	Collection	*filter_grp;
	char	searchstr[64];
	int	filterflag;
	int	filterflag2;
	int	flag;
	int	renameIndex;
};

struct SpaceAction_Runtime	{
	char	flag;
	char	_pad0[7];
};

struct SpaceLink;
struct rctf	{
	float	xmin;
	float	xmax;
	float	ymin;
	float	ymax;
};

struct rcti	{
	int	xmin;
	int	xmax;
	int	ymin;
	int	ymax;
};

struct View2D	{
	rctf	tot;
	rctf	cur;
	rcti	vert;
	rcti	hor;
	rcti	mask;
	float	min[2];
	float	max[2];
	float	minzoom;
	float	maxzoom;
	short	scroll;
	short	scroll_ui;
	short	keeptot;
	short	keepzoom;
	short	keepofs;
	short	flag;
	short	align;
	short	winx;
	short	winy;
	short	oldwinx;
	short	oldwiny;
	short	around;
	char	alpha_vert;
	char	alpha_hor;
	char	_pad[2];
	float	page_size_y;
	void	*sms;
	void	*smooth_timer;
};

struct SpaceAction	{
	SpaceLink	*next;
	SpaceLink	*prev;
	ListBase	regionbase;
	char	spacetype;
	char	link_flag;
	char	_pad0[6];
	View2D	v2d;
	bAction	*action;
	int	action_slot_handle;
	char	_pad2[4];
	bDopeSheet	ads;
	float	timeslide;
	short	flag;
	char	mode;
	char	mode_prev;
	char	autosnap;
	char	cache_display;
	char	_pad1[6];
	SpaceAction_Runtime	runtime;
};

struct Ipo;
struct bActionChannel	{
	bActionChannel	*next;
	bActionChannel	*prev;
	bActionGroup	*grp;
	Ipo	*ipo;
	ListBase	constraintChannels;
	int	flag;
	char	name[64];
	int	temp;
};

struct ActionStrip;
struct ActionLayer	{
	char	name[64];
	float	influence;
	uchar	layer_flags;
	int8_t	layer_mix_mode;
	uchar	_pad0[2];
	ActionStrip	**strip_array;
	int	strip_array_num;
	uchar	_pad1[4];
};

struct ActionSlot	{
	char	name[66];
	short	idtype;
	int	handle;
	int8_t	slot_flags;
	uchar	_pad1[7];
	void	*runtime;
};

struct ActionStrip	{
	int8_t	strip_type;
	uchar	_pad0[3];
	int	data_index;
	float	frame_start;
	float	frame_end;
	float	frame_offset;
	uchar	_pad1[4];
};

struct ActionStripKeyframeData	{
	ActionChannelBag	**channelbag_array;
	int	channelbag_array_num;
	uchar	_pad[4];
};

struct FCurve;
struct ActionChannelBag	{
	int	slot_handle;
	int	group_array_num;
	bActionGroup	**group_array;
	uchar	_pad[4];
	int	fcurve_array_num;
	FCurve	**fcurve_array;
};

struct FModifier	{
	FModifier	*next;
	FModifier	*prev;
	FCurve	*curve;
	void	*data;
	char	name[64];
	short	type;
	short	flag;
	short	ui_expand_flag;
	char	_pad[6];
	float	influence;
	float	sfra;
	float	efra;
	float	blendin;
	float	blendout;
};

struct FMod_Generator	{
	float	*coefficients;
	int	arraysize;
	int	poly_order;
	int	mode;
	int	flag;
};

struct FMod_FunctionGenerator	{
	float	amplitude;
	float	phase_multiplier;
	float	phase_offset;
	float	value_offset;
	int	type;
	int	flag;
};

struct FCM_EnvelopeData	{
	float	min;
	float	max;
	float	time;
	short	f1;
	short	f2;
};

struct FMod_Envelope	{
	FCM_EnvelopeData	*data;
	int	totvert;
	float	midval;
	float	min;
	float	max;
};

struct FMod_Cycles	{
	short	before_mode;
	short	after_mode;
	short	before_cycles;
	short	after_cycles;
};

struct FMod_Limits	{
	rctf	rect;
	int	flag;
	char	_pad[4];
};

struct FMod_Noise	{
	float	size;
	float	strength;
	float	phase;
	float	offset;
	float	roughness;
	float	lacunarity;
	short	depth;
	short	modification;
	char	legacy_noise;
	char	_pad[3];
};

struct FMod_Stepped	{
	float	step_size;
	float	offset;
	float	start_frame;
	float	end_frame;
	int	flag;
};

struct DriverTarget	{
	ID	*id;
	char	*rna_path;
	char	pchan_name[64];
	short	transChan;
	char	rotation_mode;
	char	_pad[5];
	short	flag;
	short	options;
	int	idtype;
	int	context_property;
	float	fallback_value;
};

struct DriverVar	{
	DriverVar	*next;
	DriverVar	*prev;
	char	name[64];
	DriverTarget	targets[8];
	char	num_targets;
	char	type;
	short	flag;
	float	curval;
};

struct ChannelDriver	{
	ListBase	variables;
	char	expression[256];
	void	*expr_comp;
	void	*expr_simple;
	float	curval;
	float	influence;
	int	type;
	int	flag;
};

struct FPoint	{
	float	vec[2];
	int	flag;
	char	_pad[4];
};

struct BezTriple;
struct FCurve	{
	FCurve	*next;
	FCurve	*prev;
	bActionGroup	*grp;
	ChannelDriver	*driver;
	ListBase	modifiers;
	BezTriple	*bezt;
	FPoint	*fpt;
	int	totvert;
	int	active_keyframe_index;
	float	curval;
	short	flag;
	short	extend;
	char	auto_smoothing;
	char	_pad[3];
	int	array_index;
	char	*rna_path;
	int	color_mode;
	float	color[3];
	float	prev_norm_factor;
	float	prev_offset;
};

struct NlaStrip	{
	NlaStrip	*next;
	NlaStrip	*prev;
	ListBase	strips;
	bAction	*act;
	int	action_slot_handle;
	char	action_slot_name[66];
	char	_pad0[2];
	ListBase	fcurves;
	ListBase	modifiers;
	char	name[64];
	float	influence;
	float	strip_time;
	float	start;
	float	end;
	float	actstart;
	float	actend;
	float	repeat;
	float	scale;
	float	blendin;
	float	blendout;
	short	blendmode;
	short	extendmode;
	char	_pad1[2];
	short	type;
	void	*speaker_handle;
	int	flag;
	char	_pad2[4];
	NlaStrip	*orig_strip;
	void	*_pad3;
};

struct NlaTrack	{
	NlaTrack	*next;
	NlaTrack	*prev;
	ListBase	strips;
	int	flag;
	int	index;
	char	name[64];
};

struct KS_Path	{
	KS_Path	*next;
	KS_Path	*prev;
	ID	*id;
	char	group[64];
	int	idtype;
	short	groupmode;
	short	flag;
	char	*rna_path;
	int	array_index;
	short	keyingflag;
	short	keyingoverride;
};

struct KeyingSet	{
	KeyingSet	*next;
	KeyingSet	*prev;
	ListBase	paths;
	char	idname[64];
	char	name[64];
	char	description[1024];
	char	typeinfo[64];
	int	active_path;
	short	flag;
	short	keyingflag;
	short	keyingoverride;
	char	_pad[6];
};

struct AnimOverride	{
	AnimOverride	*next;
	AnimOverride	*prev;
	char	*rna_path;
	int	array_index;
	float	value;
};

struct AnimData	{
	bAction	*action;
	int	slot_handle;
	char	slot_name[66];
	uchar	_pad0[2];
	bAction	*tmpact;
	int	tmp_slot_handle;
	char	tmp_slot_name[66];
	uchar	_pad1[2];
	ListBase	nla_tracks;
	NlaTrack	*act_track;
	NlaStrip	*actstrip;
	ListBase	drivers;
	ListBase	overrides;
	FCurve	**driver_array;
	int	flag;
	short	act_blendmode;
	short	act_extendmode;
	float	act_influence;
	uchar	_pad2[4];
};

struct IdAdtTemplate	{
	ID	id;
	AnimData	*adt;
};

struct Bone_Runtime	{
	ListBase	collections;
};

struct Bone	{
	Bone	*next;
	Bone	*prev;
	IDProperty	*prop;
	IDProperty	*system_properties;
	void	*_pad0;
	Bone	*parent;
	ListBase	childbase;
	char	name[64];
	float	roll;
	float	head[3];
	float	tail[3];
	float	bone_mat[3][3];
	int	flag;
	int8_t	drawtype;
	char	_pad1[3];
	BoneColor	color;
	char	inherit_scale_mode;
	char	_pad[3];
	float	arm_head[3];
	float	arm_tail[3];
	float	arm_mat[4][4];
	float	arm_roll;
	float	dist;
	float	weight;
	float	xwidth;
	float	length;
	float	zwidth;
	float	rad_head;
	float	rad_tail;
	float	roll1;
	float	roll2;
	float	curveInX;
	float	curveInY;
	float	curveOutX;
	float	curveOutY;
	float	ease1;
	float	ease2;
	float	scaleIn;
	float	scale_in_y;
	float	scaleOut;
	float	scale_out_y;
	float	scale_in[3];
	float	scale_out[3];
	float	size[3];
	int	layer;
	short	segments;
	char	bbone_mapping_mode;
	char	_pad2[7];
	char	bbone_prev_type;
	char	bbone_next_type;
	int	bbone_flag;
	short	bbone_prev_flag;
	short	bbone_next_flag;
	Bone	*bbone_prev;
	Bone	*bbone_next;
	Bone_Runtime	runtime;
};

struct BoneCollection;
struct bArmature_Runtime	{
	int	active_collection_index;
	uchar	_pad0[4];
	BoneCollection	*active_collection;
};

struct bArmature	{
	ID	id;
	AnimData	*adt;
	ListBase	bonebase;
	void	*bonehash;
	void	*_pad1;
	ListBase	*edbo;
	Bone	*act_bone;
	void	*act_edbone;
	char	needs_flush_to_id;
	char	_pad0[3];
	int	flag;
	int	drawtype;
	short	deformflag;
	short	pathflag;
	ListBase	collections;
	BoneCollection	**collection_array;
	int	collection_array_num;
	int	collection_root_count;
	char	active_collection_name[64];
	int	layer_used;
	int	layer;
	int	layer_protected;
	float	axes_position;
	bArmature_Runtime	runtime;
};

struct BoneCollection	{
	BoneCollection	*next;
	BoneCollection	*prev;
	char	name[64];
	ListBase	bones;
	uchar	flags;
	uchar	_pad0[7];
	int	child_index;
	int	child_count;
	IDProperty	*prop;
	IDProperty	*system_properties;
};

struct BoneCollectionMember	{
	BoneCollectionMember	*next;
	BoneCollectionMember	*prev;
	Bone	*bone;
};

struct BoneCollectionReference	{
	BoneCollectionReference	*next;
	BoneCollectionReference	*prev;
	BoneCollection	*bcoll;
};

struct AssetTag	{
	AssetTag	*next;
	AssetTag	*prev;
	char	name[64];
};

struct bUUID	{
	int	time_low;
	ushort	time_mid;
	ushort	time_hi_and_version;
	uchar	clock_seq_hi_and_reserved;
	uchar	clock_seq_low;
	uchar	node[6];
};

struct AssetMetaData	{
	void	*local_type_info;
	IDProperty	*properties;
	bUUID	catalog_id;
	char	catalog_simple_name[64];
	char	*author;
	char	*description;
	char	*copyright;
	char	*license;
	ListBase	tags;
	short	active_tag;
	short	tot_tags;
	char	_pad[4];
};

struct AssetLibraryReference	{
	short	type;
	char	_pad1[2];
	int	custom_library_index;
};

struct AssetWeakReference	{
	char	_pad[6];
	short	asset_library_type;
	char	*asset_library_identifier;
	char	*relative_asset_identifier;
};

struct AssetCatalogPathLink	{
	AssetCatalogPathLink	*next;
	AssetCatalogPathLink	*prev;
	char	*path;
};

struct AttributeArray	{
	void	*data;
	void	*sharing_info;
	int64_t	size;
};

struct AttributeSingle	{
	void	*data;
	void	*sharing_info;
};

struct Attribute	{
	char	*name;
	short	data_type;
	int8_t	domain;
	int8_t	storage_type;
	char	_pad[4];
	void	*data;
};

struct AttributeStorage	{
	Attribute	*dna_attributes;
	int	dna_attributes_num;
	char	_pad[4];
	void	*runtime;
};

struct BoidRule	{
	BoidRule	*next;
	BoidRule	*prev;
	int	type;
	int	flag;
	char	name[32];
};

struct BoidRuleGoalAvoid	{
	BoidRule	rule;
	Object	*ob;
	int	options;
	float	fear_factor;
	int	signal_id;
	int	channels;
};

struct BoidRuleAvoidCollision	{
	BoidRule	rule;
	int	options;
	float	look_ahead;
};

struct BoidRuleFollowLeader	{
	BoidRule	rule;
	Object	*ob;
	float	loc[3];
	float	oloc[3];
	float	cfra;
	float	distance;
	int	options;
	int	queue_size;
};

struct BoidRuleAverageSpeed	{
	BoidRule	rule;
	float	wander;
	float	level;
	float	speed;
	char	_pad0[4];
};

struct BoidRuleFight	{
	BoidRule	rule;
	float	distance;
	float	flee_distance;
};

struct BoidData	{
	float	health;
	float	acc[3];
	short	state_id;
	short	mode;
};

struct BoidState	{
	BoidState	*next;
	BoidState	*prev;
	ListBase	rules;
	ListBase	conditions;
	ListBase	actions;
	char	name[32];
	int	id;
	int	flag;
	int	ruleset_type;
	float	rule_fuzziness;
	int	signal_id;
	int	channels;
	float	volume;
	float	falloff;
};

struct BoidSettings	{
	int	options;
	int	last_state_id;
	float	landing_smoothness;
	float	height;
	float	banking;
	float	pitch;
	float	health;
	float	aggression;
	float	strength;
	float	accuracy;
	float	range;
	float	air_min_speed;
	float	air_max_speed;
	float	air_max_acc;
	float	air_max_ave;
	float	air_personal_space;
	float	land_jump_speed;
	float	land_max_speed;
	float	land_max_acc;
	float	land_max_ave;
	float	land_personal_space;
	float	land_stick_force;
	ListBase	states;
};

struct CurveMapping;
struct Material;
struct BrushGpencilSettings	{
	float	draw_smoothfac;
	float	fill_factor;
	float	draw_strength;
	float	draw_jitter;
	float	draw_angle;
	float	draw_angle_factor;
	float	draw_random_press;
	float	draw_random_strength;
	short	draw_smoothlvl;
	short	draw_subdivide;
	short	fill_layer_mode;
	short	fill_direction;
	float	fill_threshold;
	char	_pad2[2];
	int8_t	caps_type;
	char	_pad[1];
	int	flag2;
	int	fill_simplylvl;
	int	fill_draw_mode;
	int	fill_extend_mode;
	int	input_samples;
	float	uv_random;
	int	brush_type;
	int	eraser_mode;
	float	active_smooth;
	float	era_strength_f;
	float	era_thickness_f;
	int	flag;
	float	gradient_f;
	float	gradient_s[2];
	float	simplify_f;
	float	vertex_factor;
	int	vertex_mode;
	int	sculpt_flag;
	int	sculpt_mode_flag;
	short	preset_type;
	short	brush_draw_mode;
	float	random_hue;
	float	random_saturation;
	float	random_value;
	int	color_jitter_flag;
	char	_pad1[4];
	float	fill_extend_fac;
	int	dilate_pixels;
	CurveMapping	*curve_sensitivity;
	CurveMapping	*curve_strength;
	CurveMapping	*curve_jitter;
	CurveMapping	*curve_rand_pressure;
	CurveMapping	*curve_rand_strength;
	CurveMapping	*curve_rand_uv;
	CurveMapping	*curve_rand_hue;
	CurveMapping	*curve_rand_saturation;
	CurveMapping	*curve_rand_value;
	float	outline_fac;
	float	simplify_px;
	Material	*material;
	Material	*material_alt;
};

struct BrushCurvesSculptSettings	{
	int	add_amount;
	int	points_per_curve;
	int	flag;
	float	minimum_length;
	float	curve_length;
	float	minimum_distance;
	float	curve_radius;
	int	density_add_attempts;
	uchar	density_mode;
	char	_pad[7];
	CurveMapping	*curve_parameter_falloff;
};

struct Tex;
struct MTex	{
	short	texco;
	short	mapto;
	short	blendtype;
	char	_pad2[2];
	Object	*object;
	Tex	*tex;
	char	uvname[68];
	char	projx;
	char	projy;
	char	projz;
	char	mapping;
	char	brush_map_mode;
	char	brush_angle_mode;
	short	which_output;
	float	ofs[3];
	float	size[3];
	float	rot;
	float	random_angle;
	float	r;
	float	g;
	float	b;
	float	k;
	float	def_var;
	float	colfac;
	float	alphafac;
	float	timefac;
	float	lengthfac;
	float	clumpfac;
	float	dampfac;
	float	kinkfac;
	float	kinkampfac;
	float	roughfac;
	float	padensfac;
	float	gravityfac;
	float	lifefac;
	float	sizefac;
	float	ivelfac;
	float	fieldfac;
	float	twistfac;
};

struct ColorBand;
struct PaintCurve;
struct Brush	{
	ID	id;
	CurveMapping	*curve;
	MTex	mtex;
	MTex	mask_mtex;
	Brush	*toggle_brush;
	void	*icon_imbuf;
	PreviewImage	*preview;
	ColorBand	*gradient;
	PaintCurve	*paint_curve;
	char	icon_filepath[1024];
	float	normal_weight;
	float	rake_factor;
	short	blend;
	short	ob_mode;
	float	weight;
	int	size;
	int	flag;
	int	flag2;
	int	sampling_flag;
	int	input_samples;
	int	mask_pressure;
	float	jitter;
	int	jitter_absolute;
	int	overlay_flags;
	int	spacing;
	int	smooth_stroke_radius;
	float	smooth_stroke_factor;
	float	rate;
	float	rgb[3];
	int	color_jitter_flag;
	float	hsv_jitter[3];
	CurveMapping	*curve_rand_hue;
	CurveMapping	*curve_rand_saturation;
	CurveMapping	*curve_rand_value;
	float	alpha;
	float	hardness;
	float	flow;
	float	wet_mix;
	float	wet_persistence;
	float	density;
	int	paint_flags;
	float	tip_roundness;
	float	tip_scale_x;
	float	secondary_rgb[3];
	float	dash_ratio;
	int	dash_samples;
	int	sculpt_plane;
	float	plane_offset;
	int	gradient_spacing;
	char	gradient_stroke_mode;
	char	gradient_fill_mode;
	char	has_unsaved_changes;
	char	falloff_shape;
	float	falloff_angle;
	char	sculpt_tool;
	char	vertexpaint_tool;
	char	weightpaint_tool;
	char	imagepaint_tool;
	char	mask_tool;
	char	gpencil_tool;
	char	gpencil_vertex_tool;
	char	gpencil_sculpt_tool;
	char	gpencil_weight_tool;
	char	curves_sculpt_tool;
	char	_pad1[10];
	float	autosmooth_factor;
	float	tilt_strength_factor;
	float	topology_rake_factor;
	float	crease_pinch_factor;
	float	normal_radius_factor;
	float	area_radius_factor;
	float	wet_paint_radius_factor;
	float	plane_trim;
	float	height;
	float	plane_height;
	float	plane_depth;
	float	stabilize_normal;
	float	stabilize_plane;
	int	plane_inversion_mode;
	float	texture_sample_bias;
	int	curve_preset;
	float	disconnected_distance_max;
	int	deform_target;
	int	automasking_flags;
	int	automasking_boundary_edges_propagation_steps;
	float	automasking_start_normal_limit;
	float	automasking_start_normal_falloff;
	float	automasking_view_normal_limit;
	float	automasking_view_normal_falloff;
	int	elastic_deform_type;
	float	elastic_deform_volume_preservation;
	int	snake_hook_deform_type;
	int	pose_deform_type;
	float	pose_offset;
	int	pose_smooth_iterations;
	int	pose_ik_segments;
	int	pose_origin_type;
	int	boundary_deform_type;
	int	boundary_falloff_type;
	float	boundary_offset;
	int	cloth_deform_type;
	int	cloth_force_falloff_type;
	int	cloth_simulation_area_type;
	float	cloth_mass;
	float	cloth_damping;
	float	cloth_sim_limit;
	float	cloth_sim_falloff;
	float	cloth_constraint_softbody_strength;
	int	smooth_deform_type;
	float	surface_smooth_shape_preservation;
	float	surface_smooth_current_vertex;
	int	surface_smooth_iterations;
	float	multiplane_scrape_angle;
	int	smear_deform_type;
	int	slide_deform_type;
	int	texture_overlay_alpha;
	int	mask_overlay_alpha;
	int	cursor_overlay_alpha;
	float	unprojected_radius;
	float	sharp_threshold;
	int	blur_kernel_radius;
	int	blur_mode;
	float	fill_threshold;
	float	add_col[4];
	float	sub_col[4];
	float	stencil_pos[2];
	float	stencil_dimension[2];
	float	mask_stencil_pos[2];
	float	mask_stencil_dimension[2];
	BrushGpencilSettings	*gpencil_settings;
	BrushCurvesSculptSettings	*curves_sculpt_settings;
	int	automasking_cavity_blur_steps;
	float	automasking_cavity_factor;
	CurveMapping	*automasking_cavity_curve;
};

struct tPaletteColorHSV	{
	float	rgb[3];
	float	value;
	float	h;
	float	s;
	float	v;
};

struct PaletteColor	{
	PaletteColor	*next;
	PaletteColor	*prev;
	float	rgb[3];
	float	value;
};

struct Palette	{
	ID	id;
	ListBase	colors;
	int	active_color;
	char	_pad[4];
};

struct BezTriple	{
	float	vec[3][3];
	float	alfa;
	float	weight;
	float	radius;
	char	ipo;
	uchar	h1;
	uchar	h2;
	uchar	f1;
	uchar	f2;
	uchar	f3;
	char	hide;
	char	easing;
	float	back;
	float	amplitude;
	float	period;
	char	auto_handle_type;
	char	_pad[3];
};

struct PaintCurvePoint	{
	BezTriple	bez;
	float	pressure;
};

struct PaintCurve	{
	ID	id;
	PaintCurvePoint	*points;
	int	tot_points;
	int	add_index;
};

struct CacheObjectPath	{
	CacheObjectPath	*next;
	CacheObjectPath	*prev;
	char	path[4096];
};

struct CacheFileLayer	{
	CacheFileLayer	*next;
	CacheFileLayer	*prev;
	char	filepath[1024];
	int	flag;
	int	_pad;
};

struct CacheFile	{
	ID	id;
	AnimData	*adt;
	ListBase	object_paths;
	ListBase	layers;
	char	filepath[1024];
	char	is_sequence;
	char	forward_axis;
	char	up_axis;
	char	override_frame;
	float	scale;
	float	frame;
	float	frame_offset;
	char	_pad[4];
	short	flag;
	char	type;
	char	use_render_procedural;
	char	_pad1[3];
	char	use_prefetch;
	int	prefetch_cache_size;
	int	active_layer;
	char	_pad2[3];
	char	velocity_unit;
	char	velocity_name[64];
	void	*handle;
	char	handle_filepath[1024];
	void	*handle_readers;
};

struct CameraStereoSettings	{
	float	interocular_distance;
	float	convergence_distance;
	short	convergence_mode;
	short	pivot;
	short	flag;
	char	_pad[2];
	float	pole_merge_angle_from;
	float	pole_merge_angle_to;
};

struct Image;
struct Scene;
struct ImageUser	{
	Scene	*scene;
	int	framenr;
	int	frames;
	int	offset;
	int	sfra;
	char	cycl;
	char	multiview_eye;
	short	pass;
	int	tile;
	short	multi_index;
	short	view;
	short	layer;
	short	flag;
};

struct MovieClip;
struct MovieClipUser	{
	int	framenr;
	short	render_size;
	short	render_flag;
};

struct CameraBGImage	{
	CameraBGImage	*next;
	CameraBGImage	*prev;
	Image	*ima;
	ImageUser	iuser;
	MovieClip	*clip;
	MovieClipUser	cuser;
	float	offset[2];
	float	scale;
	float	rotation;
	float	alpha;
	short	flag;
	short	source;
};

struct CameraDOFSettings	{
	Object	*focus_object;
	char	focus_subtarget[64];
	float	focus_distance;
	float	aperture_fstop;
	float	aperture_rotation;
	float	aperture_ratio;
	int	aperture_blades;
	short	flag;
	char	_pad[2];
};

struct Camera_Runtime	{
	float	drw_corners[2][4][2];
	float	drw_tria[2][2];
	float	drw_depth[2];
	float	drw_focusmat[4][4];
	float	drw_normalmat[4][4];
};

struct Text;
struct GPUDOFSettings	{
	float	focus_distance;
	float	fstop;
	float	focal_length;
	float	sensor;
	float	rotation;
	float	ratio;
	int	num_blades;
	int	high_quality;
};

struct Camera	{
	ID	id;
	AnimData	*adt;
	char	type;
	char	dtx;
	short	flag;
	float	passepartalpha;
	float	clipsta;
	float	clipend;
	float	lens;
	float	ortho_scale;
	float	drawsize;
	float	sensor_x;
	float	sensor_y;
	float	shiftx;
	float	shifty;
	float	YF_dofdist;
	char	sensor_fit;
	char	panorama_type;
	char	_pad[2];
	float	fisheye_fov;
	float	fisheye_lens;
	float	latitude_min;
	float	latitude_max;
	float	longitude_min;
	float	longitude_max;
	float	fisheye_polynomial_k0;
	float	fisheye_polynomial_k1;
	float	fisheye_polynomial_k2;
	float	fisheye_polynomial_k3;
	float	fisheye_polynomial_k4;
	float	central_cylindrical_range_u_min;
	float	central_cylindrical_range_u_max;
	float	central_cylindrical_range_v_min;
	float	central_cylindrical_range_v_max;
	float	central_cylindrical_radius;
	float	_pad2;
	Text	*custom_shader;
	char	custom_filepath[1024];
	char	custom_bytecode_hash[64];
	char	*custom_bytecode;
	int	custom_mode;
	int	_pad3;
	Ipo	*ipo;
	Object	*dof_ob;
	GPUDOFSettings	gpu_dof;
	CameraDOFSettings	dof;
	ListBase	bg_images;
	CameraStereoSettings	stereo;
	Camera_Runtime	runtime;
};

struct EffectorWeights;
struct ClothSimSettings	{
	void	*cache;
	float	mingoal;
	float	Cdis;
	float	Cvi;
	float	gravity[3];
	float	dt;
	float	mass;
	float	structural;
	float	shear;
	float	bending;
	float	max_bend;
	float	max_struct;
	float	max_shear;
	float	max_sewing;
	float	avg_spring_len;
	float	timescale;
	float	time_scale;
	float	maxgoal;
	float	eff_force_scale;
	float	eff_wind_scale;
	float	sim_time_old;
	float	defgoal;
	float	goalspring;
	float	goalfrict;
	float	velocity_smooth;
	float	density_target;
	float	density_strength;
	float	collider_friction;
	float	vel_damping;
	float	shrink_min;
	float	shrink_max;
	float	uniform_pressure_force;
	float	target_volume;
	float	pressure_factor;
	float	fluid_density;
	short	vgroup_pressure;
	char	_pad7[6];
	float	bending_damping;
	float	voxel_cell_size;
	int	stepsPerFrame;
	int	flags;
	int	preroll;
	int	maxspringlen;
	short	solver_type;
	short	vgroup_bend;
	short	vgroup_mass;
	short	vgroup_struct;
	short	vgroup_shrink;
	short	shapekey_rest;
	short	presets;
	short	reset;
	EffectorWeights	*effector_weights;
	short	bending_model;
	short	vgroup_shear;
	float	tension;
	float	compression;
	float	max_tension;
	float	max_compression;
	float	tension_damp;
	float	compression_damp;
	float	shear_damp;
	float	internal_spring_max_length;
	float	internal_spring_max_diversion;
	short	vgroup_intern;
	char	_pad1[2];
	float	internal_tension;
	float	internal_compression;
	float	max_internal_tension;
	float	max_internal_compression;
	char	_pad0[4];
};

struct ClothCollSettings	{
	void	*collision_list;
	float	epsilon;
	float	self_friction;
	float	friction;
	float	damping;
	float	selfepsilon;
	float	repel_force;
	float	distance_repel;
	int	flags;
	short	self_loop_count;
	short	loop_count;
	char	_pad[4];
	Collection	*group;
	short	vgroup_selfcol;
	short	vgroup_objcol;
	char	_pad2[4];
	float	clamp;
	float	self_clamp;
};

struct CollectionLightLinking	{
	uchar	link_state;
	uchar	_pad[3];
};

struct CollectionObject	{
	CollectionObject	*next;
	CollectionObject	*prev;
	Object	*ob;
	CollectionLightLinking	light_linking;
	int	_pad;
};

struct CollectionChild	{
	CollectionChild	*next;
	CollectionChild	*prev;
	Collection	*collection;
	CollectionLightLinking	light_linking;
	int	_pad;
};

struct CollectionExport	{
	CollectionExport	*next;
	CollectionExport	*prev;
	char	fh_idname[64];
	char	name[64];
	IDProperty	*export_properties;
	int	flag;
	int	_pad0;
};

struct Collection_Runtime	{
	ListBase	object_cache;
	ListBase	object_cache_instanced;
	ListBase	parents;
	void	*gobject_hash;
	uchar	tag;
	char	_pad0[7];
};

struct ViewLayer;
struct Collection	{
	ID	id;
	ID	*owner_id;
	ListBase	gobject;
	ListBase	children;
	char	_pad0[4];
	int	active_exporter_index;
	ListBase	exporters;
	PreviewImage	*preview;
	int	layer;
	float	dupli_ofs[3];
	uchar	flag;
	int8_t	color_tag;
	char	_pad1[2];
	uchar	lineart_usage;
	uchar	lineart_flags;
	uchar	lineart_intersection_mask;
	uchar	lineart_intersection_priority;
	ViewLayer	*view_layer;
	Collection_Runtime	runtime;
};

struct CurveMapPoint	{
	float	x;
	float	y;
	short	flag;
	short	shorty;
};

struct CurveMap	{
	short	totpoint;
	short	flag;
	float	range;
	float	mintable;
	float	maxtable;
	float	ext_in[2];
	float	ext_out[2];
	CurveMapPoint	*curve;
	CurveMapPoint	*table;
	CurveMapPoint	*premultable;
	float	premul_ext_in[2];
	float	premul_ext_out[2];
	short	default_handle_type;
	char	_pad[6];
};

struct CurveMapping	{
	int	flag;
	int	cur;
	int	preset;
	int	changed_timestamp;
	rctf	curr;
	rctf	clipr;
	CurveMap	cm[4];
	float	black[3];
	float	white[3];
	float	bwmul[3];
	float	sample[3];
	short	tone;
	char	_pad[6];
};

struct Histogram	{
	int	channels;
	int	x_resolution;
	float	data_luma[256];
	float	data_r[256];
	float	data_g[256];
	float	data_b[256];
	float	data_a[256];
	float	xmax;
	float	ymax;
	short	mode;
	short	flag;
	int	height;
	float	co[2][2];
};

struct Scopes	{
	int	ok;
	int	sample_full;
	int	sample_lines;
	int	wavefrm_mode;
	int	vecscope_mode;
	int	wavefrm_height;
	int	vecscope_height;
	int	waveform_tot;
	float	accuracy;
	float	wavefrm_alpha;
	float	wavefrm_yfac;
	float	vecscope_alpha;
	float	minmax[3][2];
	Histogram	hist;
	float	*waveform_1;
	float	*waveform_2;
	float	*waveform_3;
	float	*vecscope;
	float	*vecscope_rgb;
};

struct ColorManagedViewSettings	{
	int	flag;
	char	_pad[4];
	char	look[64];
	char	view_transform[64];
	float	exposure;
	float	gamma;
	float	temperature;
	float	tint;
	CurveMapping	*curve_mapping;
	void	*_pad2;
};

struct ColorManagedDisplaySettings	{
	char	display_device[64];
};

struct ColorManagedColorspaceSettings	{
	char	name[64];
};

struct CBData	{
	float	r;
	float	g;
	float	b;
	float	a;
	float	pos;
	int	cur;
};

struct ColorBand	{
	short	tot;
	short	cur;
	char	ipotype;
	char	ipotype_hue;
	char	color_mode;
	char	_pad[1];
	CBData	data[32];
};

struct bConstraintChannel	{
	bConstraintChannel	*next;
	bConstraintChannel	*prev;
	Ipo	*ipo;
	short	flag;
	char	name[30];
};

struct bConstraint	{
	bConstraint	*next;
	bConstraint	*prev;
	void	*data;
	short	type;
	short	flag;
	char	ownspace;
	char	tarspace;
	short	ui_expand_flag;
	Object	*space_object;
	char	space_subtarget[64];
	char	name[64];
	float	enforce;
	float	headtail;
	Ipo	*ipo;
	float	lin_error;
	float	rot_error;
};

struct bConstraintTarget	{
	bConstraintTarget	*next;
	bConstraintTarget	*prev;
	Object	*tar;
	char	subtarget[64];
	float	matrix[4][4];
	short	space;
	short	flag;
	short	type;
	short	rotOrder;
	float	weight;
	char	_pad[4];
};

struct bKinematicConstraint	{
	Object	*tar;
	short	iterations;
	short	flag;
	short	rootbone;
	short	max_rootbone;
	char	subtarget[64];
	Object	*poletar;
	char	polesubtarget[64];
	float	poleangle;
	float	weight;
	float	orientweight;
	float	grabtarget[3];
	short	type;
	short	mode;
	float	dist;
};

struct bSplineIKConstraint	{
	Object	*tar;
	float	*points;
	short	numpoints;
	short	chainlen;
	short	flag;
	short	xzScaleMode;
	short	yScaleMode;
	short	_pad[3];
	float	bulge;
	float	bulge_min;
	float	bulge_max;
	float	bulge_smooth;
};

struct bArmatureConstraint	{
	int	flag;
	char	_pad[4];
	ListBase	targets;
};

struct bTrackToConstraint	{
	Object	*tar;
	int	reserved1;
	int	reserved2;
	int	flags;
	char	_pad[4];
	char	subtarget[64];
};

struct bRotateLikeConstraint	{
	Object	*tar;
	int	flag;
	char	euler_order;
	char	mix_mode;
	char	_pad[2];
	char	subtarget[64];
};

struct bLocateLikeConstraint	{
	Object	*tar;
	int	flag;
	int	reserved1;
	char	subtarget[64];
};

struct bSizeLikeConstraint	{
	Object	*tar;
	int	flag;
	float	power;
	char	subtarget[64];
};

struct bSameVolumeConstraint	{
	char	flag;
	char	mode;
	char	_pad[2];
	float	volume;
};

struct bTransLikeConstraint	{
	Object	*tar;
	int	flag;
	char	mix_mode;
	char	_pad[3];
	char	subtarget[64];
};

struct bMinMaxConstraint	{
	Object	*tar;
	int	minmaxflag;
	float	offset;
	int	flag;
	char	subtarget[64];
	int	_pad;
};

struct bActionConstraint	{
	Object	*tar;
	short	type;
	short	local;
	int	start;
	int	end;
	float	min;
	float	max;
	int	flag;
	char	mix_mode;
	char	_pad[3];
	float	eval_time;
	bAction	*act;
	int	action_slot_handle;
	char	action_slot_name[66];
	char	_pad1[2];
	char	subtarget[64];
};

struct bLockTrackConstraint	{
	Object	*tar;
	int	trackflag;
	int	lockflag;
	char	subtarget[64];
};

struct bDampTrackConstraint	{
	Object	*tar;
	int	trackflag;
	char	_pad[4];
	char	subtarget[64];
};

struct bFollowPathConstraint	{
	Object	*tar;
	float	offset;
	float	offset_fac;
	int	followflag;
	short	trackflag;
	short	upflag;
};

struct bStretchToConstraint	{
	Object	*tar;
	int	flag;
	int	volmode;
	int	plane;
	float	orglength;
	float	bulge;
	float	bulge_min;
	float	bulge_max;
	float	bulge_smooth;
	char	subtarget[64];
};

struct bRigidBodyJointConstraint	{
	Object	*tar;
	Object	*child;
	int	type;
	float	pivX;
	float	pivY;
	float	pivZ;
	float	axX;
	float	axY;
	float	axZ;
	float	minLimit[6];
	float	maxLimit[6];
	float	extraFz;
	short	flag;
	char	_pad[6];
};

struct bClampToConstraint	{
	Object	*tar;
	int	flag;
	int	flag2;
};

struct bChildOfConstraint	{
	Object	*tar;
	int	flag;
	char	_pad[4];
	float	invmat[4][4];
	char	subtarget[64];
};

struct bTransformConstraint	{
	Object	*tar;
	char	subtarget[64];
	short	from;
	short	to;
	char	map[3];
	char	expo;
	char	from_rotation_mode;
	char	to_euler_order;
	char	mix_mode_loc;
	char	mix_mode_rot;
	char	mix_mode_scale;
	char	_pad[3];
	float	from_min[3];
	float	from_max[3];
	float	to_min[3];
	float	to_max[3];
	float	from_min_rot[3];
	float	from_max_rot[3];
	float	to_min_rot[3];
	float	to_max_rot[3];
	float	from_min_scale[3];
	float	from_max_scale[3];
	float	to_min_scale[3];
	float	to_max_scale[3];
};

struct bPivotConstraint	{
	Object	*tar;
	char	subtarget[64];
	float	offset[3];
	short	rotAxis;
	short	flag;
};

struct bLocLimitConstraint	{
	float	xmin;
	float	xmax;
	float	ymin;
	float	ymax;
	float	zmin;
	float	zmax;
	short	flag;
	short	flag2;
};

struct bRotLimitConstraint	{
	float	xmin;
	float	xmax;
	float	ymin;
	float	ymax;
	float	zmin;
	float	zmax;
	short	flag;
	short	flag2;
	char	euler_order;
	char	_pad[3];
};

struct bSizeLimitConstraint	{
	float	xmin;
	float	xmax;
	float	ymin;
	float	ymax;
	float	zmin;
	float	zmax;
	short	flag;
	short	flag2;
};

struct bDistLimitConstraint	{
	Object	*tar;
	char	subtarget[64];
	float	dist;
	float	soft;
	short	flag;
	short	mode;
	char	_pad[4];
};

struct bShrinkwrapConstraint	{
	Object	*target;
	float	dist;
	short	shrinkType;
	char	projAxis;
	char	projAxisSpace;
	float	projLimit;
	char	shrinkMode;
	char	flag;
	char	trackAxis;
	char	_pad;
};

struct bFollowTrackConstraint	{
	MovieClip	*clip;
	char	track[64];
	int	flag;
	int	frame_method;
	char	object[64];
	Object	*camera;
	Object	*depth_ob;
};

struct bCameraSolverConstraint	{
	MovieClip	*clip;
	int	flag;
	char	_pad[4];
};

struct bObjectSolverConstraint	{
	MovieClip	*clip;
	int	flag;
	char	_pad[4];
	char	object[64];
	float	invmat[4][4];
	Object	*camera;
};

struct bTransformCacheConstraint	{
	CacheFile	*cache_file;
	char	object_path[1024];
	void	*reader;
	char	reader_object_path[1024];
};

struct BPoint	{
	float	vec[4];
	float	alfa;
	float	weight;
	uchar	f1;
	char	_pad1[1];
	short	hide;
	float	radius;
	char	_pad[4];
};

struct Nurb	{
	Nurb	*next;
	Nurb	*prev;
	short	type;
	short	mat_nr;
	short	hide;
	short	flag;
	int	pntsu;
	int	pntsv;
	char	_pad[4];
	short	resolu;
	short	resolv;
	short	orderu;
	short	orderv;
	short	flagu;
	short	flagv;
	float	*knotsu;
	float	*knotsv;
	BPoint	*bp;
	BezTriple	*bezt;
	short	tilt_interp;
	short	radius_interp;
	int	charidx;
};

struct CharInfo	{
	float	kern;
	short	mat_nr;
	char	flag;
	char	_pad[1];
};

struct TextBox	{
	float	x;
	float	y;
	float	w;
	float	h;
};

struct Key;
struct CurveProfile;
struct VFont;
struct Curves;
struct Curve	{
	ID	id;
	AnimData	*adt;
	ListBase	nurb;
	void	*editnurb;
	Object	*bevobj;
	Object	*taperobj;
	Object	*textoncurve;
	Ipo	*ipo;
	Key	*key;
	Material	**mat;
	CurveProfile	*bevel_profile;
	float	loc[3];
	float	size[3];
	short	type;
	char	texflag;
	char	_pad0[7];
	short	twist_mode;
	float	twist_smooth;
	float	smallcaps_scale;
	int	pathlen;
	short	bevresol;
	short	totcol;
	int	flag;
	float	width;
	float	ext1;
	float	ext2;
	short	resolu;
	short	resolv;
	short	resolu_ren;
	short	resolv_ren;
	int	actnu;
	int	actvert;
	char	overflow;
	char	spacemode;
	char	align_y;
	char	bevel_mode;
	char	taper_radius_mode;
	char	_pad;
	short	lines;
	float	spacing;
	float	linedist;
	float	shear;
	float	fsize;
	float	wordspace;
	float	ulpos;
	float	ulheight;
	float	xof;
	float	yof;
	float	linewidth;
	int	pos;
	int	selstart;
	int	selend;
	int	len_wchar;
	int	len;
	char	*str;
	void	*editfont;
	char	family[64];
	VFont	*vfont;
	VFont	*vfontb;
	VFont	*vfonti;
	VFont	*vfontbi;
	TextBox	*tb;
	int	totbox;
	int	actbox;
	CharInfo	*strinfo;
	CharInfo	curinfo;
	float	ctime;
	float	bevfac1;
	float	bevfac2;
	char	bevfac1_mapping;
	char	bevfac2_mapping;
	char	_pad2[6];
	float	fsize_realtime;
	Curves	*curve_eval;
	char	edit_data_from_original;
	char	_pad3[7];
	void	*batch_cache;
};

struct CurveProfilePoint	{
	float	x;
	float	y;
	short	flag;
	char	h1;
	char	h2;
	float	h1_loc[2];
	float	h2_loc[2];
	char	_pad[4];
	CurveProfile	*profile;
};

struct CurveProfile	{
	short	path_len;
	short	segments_len;
	int	preset;
	CurveProfilePoint	*path;
	CurveProfilePoint	*table;
	CurveProfilePoint	*segments;
	int	flag;
	int	changed_timestamp;
	rctf	view_rect;
	rctf	clip_rect;
};

struct CustomDataLayer;
struct CustomDataExternal;
struct CustomData	{
	CustomDataLayer	*layers;
	int	typemap[53];
	int	totlayer;
	int	maxlayer;
	int	totsize;
	void	*pool;
	CustomDataExternal	*external;
};

struct CurvesGeometry	{
	int	*curve_offsets;
	AttributeStorage	attribute_storage;
	CustomData	point_data;
	CustomData	curve_data;
	int	point_size;
	int	curve_size;
	ListBase	vertex_group_names;
	int	vertex_group_active_index;
	int	attributes_active_index;
	void	*runtime;
	float	*custom_knots;
	int	custom_knot_num;
	char	_pad[4];
};

struct Curves	{
	ID	id;
	AnimData	*adt;
	CurvesGeometry	geometry;
	int	flag;
	int	attributes_active_index;
	Material	**mat;
	short	totcol;
	char	symmetry;
	char	selection_domain;
	char	_pad[4];
	Object	*surface;
	char	*surface_uv_map;
	float	surface_collision_distance;
	char	_pad2[4];
	void	*batch_cache;
};

struct CustomDataLayer	{
	int	type;
	int	offset;
	int	flag;
	int	active;
	int	active_rnd;
	int	active_clone;
	int	active_mask;
	int	uid;
	char	name[68];
	char	_pad1[4];
	void	*data;
	void	*sharing_info;
};

struct CustomDataExternal	{
	char	filename[1024];
};

struct CustomData_MeshMasks	{
	uint64_t	vmask;
	uint64_t	emask;
	uint64_t	fmask;
	uint64_t	pmask;
	uint64_t	lmask;
};

struct DynamicPaintCanvasSettings;
struct PointCache;
struct DynamicPaintSurface	{
	DynamicPaintSurface	*next;
	DynamicPaintSurface	*prev;
	DynamicPaintCanvasSettings	*canvas;
	void	*data;
	Collection	*brush_group;
	EffectorWeights	*effector_weights;
	PointCache	*pointcache;
	ListBase	ptcaches;
	int	current_frame;
	char	name[64];
	short	format;
	short	type;
	short	disp_type;
	short	image_fileformat;
	short	effect_ui;
	short	init_color_type;
	int	flags;
	int	effect;
	int	image_resolution;
	int	substeps;
	int	start_frame;
	int	end_frame;
	float	init_color[4];
	Tex	*init_texture;
	char	init_layername[68];
	int	dry_speed;
	int	diss_speed;
	float	color_dry_threshold;
	float	depth_clamp;
	float	disp_factor;
	float	spread_speed;
	float	color_spread_speed;
	float	shrink_speed;
	float	drip_vel;
	float	drip_acc;
	float	influence_scale;
	float	radius_scale;
	float	wave_damping;
	float	wave_speed;
	float	wave_timescale;
	float	wave_spring;
	float	wave_smoothness;
	char	_pad2[4];
	char	uvlayer_name[68];
	char	image_output_path[1024];
	char	output_name[68];
	char	output_name2[68];
};

struct DynamicPaintModifierData;
struct DynamicPaintCanvasSettings	{
	DynamicPaintModifierData	*pmd;
	ListBase	surfaces;
	short	active_sur;
	short	flags;
	char	_pad[4];
	char	error[64];
};

struct ParticleSystem;
struct DynamicPaintBrushSettings	{
	DynamicPaintModifierData	*pmd;
	ParticleSystem	*psys;
	int	flags;
	int	collision;
	float	r;
	float	g;
	float	b;
	float	alpha;
	float	wetness;
	float	particle_radius;
	float	particle_smooth;
	float	paint_distance;
	ColorBand	*paint_ramp;
	ColorBand	*vel_ramp;
	short	proximity_falloff;
	short	wave_type;
	short	ray_dir;
	char	_pad[2];
	float	wave_factor;
	float	wave_clamp;
	float	max_velocity;
	float	smudge_strength;
};

struct Effect	{
	Effect	*next;
	Effect	*prev;
	short	type;
	short	flag;
	short	buttype;
	char	_pad0[2];
};

struct BuildEff	{
	BuildEff	*next;
	BuildEff	*prev;
	short	type;
	short	flag;
	short	buttype;
	char	_pad0[2];
	float	len;
	float	sfra;
};

struct PartEff	{
	PartEff	*next;
	PartEff	*prev;
	short	type;
	short	flag;
	short	buttype;
	short	stype;
	short	vertgroup;
	short	userjit;
	float	sta;
	float	end;
	float	lifetime;
	int	totpart;
	int	totkey;
	int	seed;
	float	normfac;
	float	obfac;
	float	randfac;
	float	texfac;
	float	randlife;
	float	force[3];
	float	damp;
	float	nabla;
	float	vectsize;
	float	maxlen;
	float	defvec[3];
	char	_pad[4];
	float	mult[4];
	float	life[4];
	short	child[4];
	short	mat[4];
	short	texmap;
	short	curmult;
	short	staticstep;
	short	omat;
	short	timetex;
	short	speedtex;
	short	flag2;
	short	flag2neg;
	short	disp;
	short	vertgroup_v;
	char	vgroupname[64];
	char	vgroupname_v[64];
	float	imat[4][4];
	void	*keys;
	Collection	*group;
};

struct WaveEff	{
	WaveEff	*next;
	WaveEff	*prev;
	short	type;
	short	flag;
	short	buttype;
	short	stype;
	float	startx;
	float	starty;
	float	height;
	float	width;
	float	narrow;
	float	speed;
	float	minfac;
	float	damp;
	float	timeoffs;
	float	lifetime;
};

struct bScreen;
struct FileGlobal	{
	char	subvstr[4];
	short	subversion;
	short	minversion;
	short	minsubversion;
	char	_pad[6];
	bScreen	*curscreen;
	Scene	*curscene;
	ViewLayer	*cur_view_layer;
	void	*_pad1;
	int	fileflags;
	int	globalf;
	uint64_t	build_commit_timestamp;
	char	build_hash[16];
	char	filename[1024];
};

struct FluidModifierData;
struct FluidDomainSettings	{
	FluidModifierData	*fmd;
	void	*fluid;
	void	*fluid_old;
	void	*fluid_mutex;
	Collection	*fluid_group;
	Collection	*force_group;
	Collection	*effector_group;
	void	*tex_density;
	void	*tex_color;
	void	*tex_wt;
	void	*tex_shadow;
	void	*tex_flame;
	void	*tex_flame_coba;
	void	*tex_coba;
	void	*tex_field;
	void	*tex_velocity_x;
	void	*tex_velocity_y;
	void	*tex_velocity_z;
	void	*tex_flags;
	void	*tex_range_field;
	Object	*guiding_parent;
	EffectorWeights	*effector_weights;
	float	p0[3];
	float	p1[3];
	float	dp0[3];
	float	cell_size[3];
	float	global_size[3];
	float	prev_loc[3];
	int	shift[3];
	float	shift_f[3];
	float	obj_shift_f[3];
	float	imat[4][4];
	float	obmat[4][4];
	float	fluidmat[4][4];
	float	fluidmat_wt[4][4];
	int	base_res[3];
	int	res_min[3];
	int	res_max[3];
	int	res[3];
	int	total_cells;
	float	dx;
	float	scale;
	int	boundary_width;
	float	gravity_final[3];
	int	adapt_margin;
	int	adapt_res;
	float	adapt_threshold;
	int	maxres;
	int	solver_res;
	int	border_collisions;
	int	flags;
	float	gravity[3];
	int	active_fields;
	short	type;
	char	_pad2[6];
	float	alpha;
	float	beta;
	int	diss_speed;
	float	vorticity;
	float	active_color[3];
	int	highres_sampling;
	float	burning_rate;
	float	flame_smoke;
	float	flame_vorticity;
	float	flame_ignition;
	float	flame_max_temp;
	float	flame_smoke_color[3];
	float	noise_strength;
	float	noise_pos_scale;
	float	noise_time_anim;
	int	res_noise[3];
	int	noise_scale;
	char	_pad3[4];
	float	particle_randomness;
	int	particle_number;
	int	particle_minimum;
	int	particle_maximum;
	float	particle_radius;
	float	particle_band_width;
	float	fractions_threshold;
	float	fractions_distance;
	float	flip_ratio;
	int	sys_particle_maximum;
	short	simulation_method;
	char	_pad4[6];
	float	viscosity_value;
	char	_pad5[4];
	float	surface_tension;
	float	viscosity_base;
	int	viscosity_exponent;
	float	mesh_concave_upper;
	float	mesh_concave_lower;
	float	mesh_particle_radius;
	int	mesh_smoothen_pos;
	int	mesh_smoothen_neg;
	int	mesh_scale;
	short	mesh_generator;
	char	_pad6[2];
	int	particle_type;
	int	particle_scale;
	float	sndparticle_tau_min_wc;
	float	sndparticle_tau_max_wc;
	float	sndparticle_tau_min_ta;
	float	sndparticle_tau_max_ta;
	float	sndparticle_tau_min_k;
	float	sndparticle_tau_max_k;
	int	sndparticle_k_wc;
	int	sndparticle_k_ta;
	float	sndparticle_k_b;
	float	sndparticle_k_d;
	float	sndparticle_l_min;
	float	sndparticle_l_max;
	int	sndparticle_potential_radius;
	int	sndparticle_update_radius;
	char	sndparticle_boundary;
	char	sndparticle_combined_export;
	char	_pad7[6];
	float	guiding_alpha;
	int	guiding_beta;
	float	guiding_vel_factor;
	int	guide_res[3];
	short	guiding_source;
	char	_pad8[2];
	int	cache_frame_start;
	int	cache_frame_end;
	int	cache_frame_pause_data;
	int	cache_frame_pause_noise;
	int	cache_frame_pause_mesh;
	int	cache_frame_pause_particles;
	int	cache_frame_pause_guiding;
	int	cache_frame_offset;
	int	cache_flag;
	char	cache_mesh_format;
	char	cache_data_format;
	char	cache_particle_format;
	char	cache_noise_format;
	char	cache_directory[1024];
	char	error[64];
	short	cache_type;
	char	cache_id[4];
	char	_pad9[2];
	float	dt;
	float	time_total;
	float	time_per_frame;
	float	frame_length;
	float	time_scale;
	float	cfl_condition;
	int	timesteps_minimum;
	int	timesteps_maximum;
	float	slice_per_voxel;
	float	slice_depth;
	float	display_thickness;
	float	grid_scale;
	ColorBand	*coba;
	float	vector_scale;
	float	gridlines_lower_bound;
	float	gridlines_upper_bound;
	float	gridlines_range_color[4];
	char	axis_slice_method;
	char	slice_axis;
	char	show_gridlines;
	char	draw_velocity;
	char	vector_draw_type;
	char	vector_field;
	char	vector_scale_with_magnitude;
	char	vector_draw_mac_components;
	char	use_coba;
	char	coba_field;
	char	interp_method;
	char	gridlines_color_field;
	char	gridlines_cell_filter;
	char	_pad10[3];
	float	velocity_scale;
	int	openvdb_compression;
	float	clipping;
	char	openvdb_data_depth;
	char	_pad11[7];
	int	viewsettings;
	char	_pad12[4];
	PointCache	*point_cache[2];
	ListBase	ptcaches[2];
	int	cache_comp;
	int	cache_high_comp;
	char	cache_file_format;
	char	_pad13[7];
};

struct Mesh;
struct FluidFlowSettings	{
	FluidModifierData	*fmd;
	Mesh	*mesh;
	ParticleSystem	*psys;
	Tex	*noise_texture;
	float	*verts_old;
	int	numverts;
	float	vel_multi;
	float	vel_normal;
	float	vel_random;
	float	vel_coord[3];
	char	_pad1[4];
	float	density;
	float	color[3];
	float	fuel_amount;
	float	temperature;
	float	volume_density;
	float	surface_distance;
	float	particle_size;
	int	subframes;
	float	texture_size;
	float	texture_offset;
	char	_pad2[4];
	char	uvlayer_name[68];
	char	_pad3[4];
	short	vgroup_density;
	short	type;
	short	behavior;
	short	source;
	short	texture_type;
	short	_pad4[3];
	int	flags;
};

struct FluidEffectorSettings	{
	FluidModifierData	*fmd;
	Mesh	*mesh;
	float	*verts_old;
	int	numverts;
	float	surface_distance;
	int	flags;
	int	subframes;
	short	type;
	char	_pad1[6];
	float	vel_multi;
	short	guiding_mode;
	char	_pad2[2];
};

struct FreestyleLineStyle;
struct FreestyleLineSet	{
	FreestyleLineSet	*next;
	FreestyleLineSet	*prev;
	char	name[64];
	int	flags;
	int	selection;
	short	qi;
	char	_pad1[2];
	int	qi_start;
	int	qi_end;
	int	edge_types;
	int	exclude_edge_types;
	char	_pad2[4];
	Collection	*group;
	FreestyleLineStyle	*linestyle;
};

struct FreestyleModuleConfig	{
	FreestyleModuleConfig	*next;
	FreestyleModuleConfig	*prev;
	Text	*script;
	short	is_displayed;
	char	_pad[6];
};

struct FreestyleConfig	{
	ListBase	modules;
	int	mode;
	int	raycasting_algorithm;
	int	flags;
	float	sphere_radius;
	float	dkr_epsilon;
	float	crease_angle;
	ListBase	linesets;
};

struct bGPDspoint	{
	float	x;
	float	y;
	float	z;
	float	pressure;
	float	strength;
	float	time;
	int	flag;
	float	uv_fac;
	float	uv_rot;
	float	uv_fill[2];
	float	vert_color[4];
	char	_pad2[4];
};

struct bGPDtriangle	{
	int	verts[3];
};

struct bGPDpalettecolor	{
	bGPDpalettecolor	*next;
	bGPDpalettecolor	*prev;
	char	info[64];
	float	color[4];
	float	fill[4];
	short	flag;
	char	_pad[6];
};

struct bGPDpalette	{
	bGPDpalette	*next;
	bGPDpalette	*prev;
	ListBase	colors;
	char	info[64];
	short	flag;
	char	_pad[6];
};

struct bGPDcurve_point	{
	BezTriple	bezt;
	float	pressure;
	float	strength;
	int	point_index;
	int	flag;
	float	uv_fac;
	float	uv_rot;
	float	uv_fill[2];
	float	vert_color[4];
	char	_pad[4];
};

struct bGPDcurve	{
	bGPDcurve_point	*curve_points;
	int	tot_curve_points;
	short	flag;
	char	_pad[2];
};

struct bGPDstroke;
struct bGPDstroke_Runtime	{
	char	tmp_layerinfo[128];
	float	multi_frame_falloff;
	int	stroke_start;
	int	fill_start;
	int	vertex_start;
	int	curve_start;
	int	_pad0;
	bGPDstroke	*gps_orig;
	void	*_pad2;
};

struct MDeformVert;
struct bGPDstroke	{
	bGPDstroke	*next;
	bGPDstroke	*prev;
	bGPDspoint	*points;
	bGPDtriangle	*triangles;
	int	totpoints;
	int	tot_triangles;
	short	thickness;
	short	flag;
	short	_pad[2];
	double	inittime;
	char	colorname[128];
	int	mat_nr;
	short	caps[2];
	float	gradient_f;
	float	gradient_s[2];
	float	fill_opacity_fac;
	float	uv_rotation;
	float	uv_translation[2];
	float	uv_scale;
	int	select_index;
	char	_pad4[4];
	MDeformVert	*dvert;
	void	*_pad3;
	float	vert_color_fill[4];
	bGPDcurve	*editcurve;
	bGPDstroke_Runtime	runtime;
	void	*_pad5;
};

struct bGPDframe_Runtime	{
	int	frameid;
	int	onion_id;
};

struct bGPDframe	{
	bGPDframe	*next;
	bGPDframe	*prev;
	ListBase	strokes;
	int	framenum;
	short	flag;
	short	key_type;
	bGPDframe_Runtime	runtime;
};

struct bGPDlayer_Mask	{
	bGPDlayer_Mask	*next;
	bGPDlayer_Mask	*prev;
	char	name[128];
	short	flag;
	short	sort_index;
	char	_pad[4];
};

struct bGPDlayer_Runtime	{
	int	icon_id;
	char	_pad[4];
};

struct bGPDlayer	{
	bGPDlayer	*next;
	bGPDlayer	*prev;
	ListBase	frames;
	bGPDframe	*actframe;
	short	flag;
	short	onion_flag;
	float	color[4];
	float	fill[4];
	char	info[128];
	short	thickness;
	short	pass_index;
	Object	*parent;
	float	inverse[4][4];
	char	parsubstr[64];
	short	partype;
	short	line_change;
	float	tintcolor[4];
	float	opacity;
	char	viewlayername[64];
	int	blend_mode;
	float	vertex_paint_opacity;
	short	gstep;
	short	gstep_next;
	float	gcolor_prev[3];
	float	gcolor_next[3];
	char	_pad1[4];
	ListBase	mask_layers;
	int	act_mask;
	char	_pad2[4];
	float	location[3];
	float	rotation[3];
	float	scale[3];
	float	layer_mat[4][4];
	float	layer_invmat[4][4];
	char	_pad3[4];
	bGPDlayer_Runtime	runtime;
};

struct bGPdata_Runtime	{
	void	*sbuffer;
	short	playing;
	short	matid;
	short	sbuffer_sflag;
	char	_pad1[2];
	int	sbuffer_used;
	int	sbuffer_size;
	float	vert_color_fill[4];
	float	fill_opacity_fac;
	float	arrow_start[8];
	float	arrow_end[8];
	int	arrow_start_style;
	int	arrow_end_style;
	char	_pad[4];
};

struct bGPgrid	{
	float	color[3];
	float	scale[2];
	float	offset[2];
	char	_pad1[4];
	int	lines;
	char	_pad[4];
};

struct bGPdata	{
	ID	id;
	AnimData	*adt;
	ListBase	layers;
	int	flag;
	int	curve_edit_resolution;
	float	curve_edit_threshold;
	float	curve_edit_corner_angle;
	ListBase	palettes;
	ListBase	vertex_group_names;
	float	pixfactor;
	float	line_color[4];
	float	onion_factor;
	int	onion_mode;
	int	onion_flag;
	short	gstep;
	short	gstep_next;
	float	gcolor_prev[3];
	float	gcolor_next[3];
	float	zdepth_offset;
	Material	**mat;
	short	totcol;
	short	totlayer;
	short	totframe;
	char	_pad2[6];
	int	totstroke;
	int	totpoint;
	short	draw_mode;
	short	onion_keytype;
	int	select_last_index;
	int	vertex_group_active_index;
	bGPgrid	grid;
	bGPdata_Runtime	runtime;
};

struct GpencilModifierData	{
	GpencilModifierData	*next;
	GpencilModifierData	*prev;
	int	type;
	int	mode;
	char	_pad0[4];
	short	flag;
	short	ui_expand_flag;
	char	name[64];
	char	*error;
};

struct NoiseGpencilModifierData	{
	GpencilModifierData	modifier;
	Material	*material;
	char	layername[64];
	char	materialname[64];
	char	vgname[64];
	int	pass_index;
	int	flag;
	float	factor;
	float	factor_strength;
	float	factor_thickness;
	float	factor_uvs;
	float	noise_scale;
	float	noise_offset;
	short	noise_mode;
	char	_pad[2];
	int	step;
	int	layer_pass;
	int	seed;
	CurveMapping	*curve_intensity;
};

struct SubdivGpencilModifierData	{
	GpencilModifierData	modifier;
	Material	*material;
	char	layername[64];
	char	materialname[64];
	int	pass_index;
	int	flag;
	int	level;
	int	layer_pass;
	short	type;
	char	_pad[6];
};

struct ThickGpencilModifierData	{
	GpencilModifierData	modifier;
	Material	*material;
	char	layername[64];
	char	materialname[64];
	char	vgname[64];
	int	pass_index;
	int	flag;
	float	thickness_fac;
	int	thickness;
	int	layer_pass;
	char	_pad[4];
	CurveMapping	*curve_thickness;
};

struct TimeGpencilModifierData;
struct TimeGpencilModifierSegment	{
	char	name[64];
	TimeGpencilModifierData	*gpmd;
	int	seg_start;
	int	seg_end;
	int	seg_mode;
	int	seg_repeat;
};

struct TimeGpencilModifierData	{
	GpencilModifierData	modifier;
	Material	*material;
	char	layername[64];
	int	layer_pass;
	int	flag;
	int	offset;
	float	frame_scale;
	int	mode;
	int	sfra;
	int	efra;
	char	_pad[4];
	TimeGpencilModifierSegment	*segments;
	int	segments_len;
	int	segment_active_index;
};

struct ColorGpencilModifierData	{
	GpencilModifierData	modifier;
	Material	*material;
	char	layername[64];
	char	materialname[64];
	int	pass_index;
	int	flag;
	float	hsv[3];
	char	modify_color;
	char	_pad[3];
	int	layer_pass;
	char	_pad1[4];
	CurveMapping	*curve_intensity;
};

struct OpacityGpencilModifierData	{
	GpencilModifierData	modifier;
	Material	*material;
	char	layername[64];
	char	materialname[64];
	char	vgname[64];
	int	pass_index;
	int	flag;
	float	factor;
	char	modify_color;
	char	_pad[3];
	int	layer_pass;
	float	hardeness;
	CurveMapping	*curve_intensity;
};

struct OutlineGpencilModifierData	{
	GpencilModifierData	modifier;
	Object	*object;
	Material	*material;
	char	layername[64];
	int	pass_index;
	int	flag;
	int	thickness;
	float	sample_length;
	int	subdiv;
	int	layer_pass;
	Material	*outline_material;
};

struct ArrayGpencilModifierData	{
	GpencilModifierData	modifier;
	Object	*object;
	Material	*material;
	int	count;
	int	flag;
	float	offset[3];
	float	shift[3];
	float	rnd_offset[3];
	float	rnd_rot[3];
	float	rnd_scale[3];
	char	_pad[4];
	int	seed;
	int	pass_index;
	char	layername[64];
	char	materialname[64];
	int	mat_rpl;
	int	layer_pass;
};

struct BuildGpencilModifierData	{
	GpencilModifierData	modifier;
	Material	*material;
	char	layername[64];
	int	pass_index;
	char	materialname[64];
	int	layer_pass;
	float	start_frame;
	float	end_frame;
	float	start_delay;
	float	length;
	short	flag;
	short	mode;
	short	transition;
	short	time_alignment;
	float	speed_fac;
	float	speed_maxgap;
	short	time_mode;
	char	_pad[6];
	Object	*object;
	float	percentage_fac;
	float	fade_fac;
	char	target_vgname[64];
	float	fade_opacity_strength;
	float	fade_thickness_strength;
};

struct LatticeGpencilModifierData	{
	GpencilModifierData	modifier;
	Object	*object;
	Material	*material;
	char	layername[64];
	char	materialname[64];
	char	vgname[64];
	int	pass_index;
	int	flag;
	float	strength;
	int	layer_pass;
	void	*cache_data;
};

struct LengthGpencilModifierData	{
	GpencilModifierData	modifier;
	Material	*material;
	char	layername[64];
	int	pass_index;
	int	flag;
	int	layer_pass;
	float	start_fac;
	float	end_fac;
	float	rand_start_fac;
	float	rand_end_fac;
	float	rand_offset;
	float	overshoot_fac;
	int	seed;
	int	step;
	int	mode;
	char	_pad[4];
	float	point_density;
	float	segment_influence;
	float	max_angle;
};

struct DashGpencilModifierData;
struct DashGpencilModifierSegment	{
	char	name[64];
	DashGpencilModifierData	*dmd;
	int	dash;
	int	gap;
	float	radius;
	float	opacity;
	int	mat_nr;
	int	flag;
};

struct DashGpencilModifierData	{
	GpencilModifierData	modifier;
	Material	*material;
	char	layername[64];
	int	pass_index;
	int	flag;
	int	layer_pass;
	int	dash_offset;
	DashGpencilModifierSegment	*segments;
	int	segments_len;
	int	segment_active_index;
};

struct MirrorGpencilModifierData	{
	GpencilModifierData	modifier;
	Object	*object;
	Material	*material;
	char	layername[64];
	char	materialname[64];
	int	pass_index;
	int	flag;
	int	layer_pass;
	char	_pad[4];
};

struct HookGpencilModifierData	{
	GpencilModifierData	modifier;
	Object	*object;
	Material	*material;
	char	subtarget[64];
	char	layername[64];
	char	materialname[64];
	char	vgname[64];
	int	pass_index;
	int	layer_pass;
	char	_pad[4];
	int	flag;
	char	falloff_type;
	char	_pad1[3];
	float	parentinv[4][4];
	float	cent[3];
	float	falloff;
	float	force;
	CurveMapping	*curfalloff;
};

struct SimplifyGpencilModifierData	{
	GpencilModifierData	modifier;
	Material	*material;
	char	layername[64];
	char	materialname[64];
	int	pass_index;
	int	flag;
	float	factor;
	short	mode;
	short	step;
	int	layer_pass;
	float	length;
	float	sharp_threshold;
	float	distance;
};

struct OffsetGpencilModifierData	{
	GpencilModifierData	modifier;
	Material	*material;
	char	layername[64];
	char	materialname[64];
	char	vgname[64];
	int	pass_index;
	int	flag;
	float	loc[3];
	float	rot[3];
	float	scale[3];
	float	rnd_offset[3];
	float	rnd_rot[3];
	float	rnd_scale[3];
	int	seed;
	int	mode;
	int	stroke_step;
	int	stroke_start_offset;
	int	layer_pass;
	char	_pad[4];
};

struct SmoothGpencilModifierData	{
	GpencilModifierData	modifier;
	Material	*material;
	char	layername[64];
	char	materialname[64];
	char	vgname[64];
	int	pass_index;
	int	flag;
	float	factor;
	int	step;
	int	layer_pass;
	char	_pad1[4];
	CurveMapping	*curve_intensity;
};

struct ArmatureGpencilModifierData	{
	GpencilModifierData	modifier;
	short	deformflag;
	short	multi;
	int	_pad;
	Object	*object;
	float	(*vert_coords_prev)();
	char	vgname[64];
};

struct MultiplyGpencilModifierData	{
	GpencilModifierData	modifier;
	Material	*material;
	char	layername[64];
	char	materialname[64];
	int	pass_index;
	int	flag;
	int	layer_pass;
	int	flags;
	int	duplications;
	float	distance;
	float	offset;
	float	fading_center;
	float	fading_thickness;
	float	fading_opacity;
};

struct TintGpencilModifierData	{
	GpencilModifierData	modifier;
	Object	*object;
	Material	*material;
	char	layername[64];
	char	materialname[64];
	char	vgname[64];
	int	pass_index;
	int	layer_pass;
	int	flag;
	int	mode;
	float	factor;
	float	radius;
	float	rgb[3];
	int	type;
	CurveMapping	*curve_intensity;
	ColorBand	*colorband;
};

struct TextureGpencilModifierData	{
	GpencilModifierData	modifier;
	Material	*material;
	char	layername[64];
	char	materialname[64];
	char	vgname[64];
	int	pass_index;
	int	flag;
	float	uv_offset;
	float	uv_scale;
	float	fill_rotation;
	float	fill_offset[2];
	float	fill_scale;
	int	layer_pass;
	short	fit_method;
	short	mode;
	float	alignment_rotation;
	char	_pad[4];
};

struct WeightProxGpencilModifierData	{
	GpencilModifierData	modifier;
	char	target_vgname[64];
	Material	*material;
	char	layername[64];
	char	vgname[64];
	int	pass_index;
	int	flag;
	float	min_weight;
	int	layer_pass;
	float	dist_start;
	float	dist_end;
	Object	*object;
};

struct WeightAngleGpencilModifierData	{
	GpencilModifierData	modifier;
	char	target_vgname[64];
	Material	*material;
	char	layername[64];
	char	vgname[64];
	int	pass_index;
	int	flag;
	float	min_weight;
	int	layer_pass;
	short	axis;
	short	space;
	float	angle;
};

struct LineartGpencilModifierData	{
	GpencilModifierData	modifier;
	ushort	line_types;
	char	source_type;
	char	use_multiple_levels;
	short	level_start;
	short	level_end;
	Object	*source_camera;
	Object	*light_contour_object;
	Object	*source_object;
	Collection	*source_collection;
	Material	*target_material;
	char	target_layer[64];
	char	source_vertex_group[64];
	char	vgname[64];
	float	overscan;
	float	shadow_camera_fov;
	float	shadow_camera_size;
	float	shadow_camera_near;
	float	shadow_camera_far;
	float	opacity;
	short	thickness;
	char	transparency_flags;
	char	transparency_mask;
	char	intersection_mask;
	char	shadow_selection;
	char	silhouette_selection;
	char	_pad[1];
	float	crease_threshold;
	float	angle_splitting_threshold;
	float	chain_smooth_tolerance;
	float	chaining_image_threshold;
	int	calculation_flags;
	int	flags;
	float	stroke_depth_offset;
	char	level_start_override;
	char	level_end_override;
	short	edge_types_override;
	char	shadow_selection_override;
	char	shadow_use_silhouette_override;
	char	_pad2[6];
	void	*cache;
	void	*la_data_ptr;
};

struct ShrinkwrapGpencilModifierData	{
	GpencilModifierData	modifier;
	Object	*target;
	Object	*aux_target;
	Material	*material;
	char	layername[64];
	char	vgname[64];
	int	pass_index;
	int	flag;
	int	layer_pass;
	float	keep_dist;
	short	shrink_type;
	char	shrink_opts;
	char	shrink_mode;
	float	proj_limit;
	char	proj_axis;
	char	subsurf_levels;
	char	_pad[6];
	float	smooth_factor;
	int	smooth_step;
	void	*cache_data;
};

struct EnvelopeGpencilModifierData	{
	GpencilModifierData	modifier;
	Material	*material;
	char	layername[64];
	char	vgname[64];
	int	pass_index;
	int	flag;
	int	mode;
	int	mat_nr;
	float	thickness;
	float	strength;
	int	skip;
	int	layer_pass;
	int	spread;
	char	_pad[4];
};

struct GreasePencilDrawingBase	{
	int8_t	type;
	char	_pad[3];
	int	flag;
};

struct GreasePencilDrawing	{
	GreasePencilDrawingBase	base;
	CurvesGeometry	geometry;
	void	*runtime;
};

struct GreasePencil;
struct GreasePencilDrawingReference	{
	GreasePencilDrawingBase	base;
	GreasePencil	*id_reference;
};

struct GreasePencilFrame	{
	int	drawing_index;
	int	flag;
	int8_t	type;
	char	_pad[3];
};

struct GreasePencilLayerFramesMapStorage	{
	int	*keys;
	GreasePencilFrame	*values;
	int	size;
	int	flag;
};

struct GreasePencilLayerMask	{
	GreasePencilLayerMask	*next;
	GreasePencilLayerMask	*prev;
	char	*layer_name;
	ushort	flag;
	char	_pad[6];
};

struct GreasePencilLayerTreeGroup;
struct GreasePencilLayerTreeNode	{
	GreasePencilLayerTreeNode	*next;
	GreasePencilLayerTreeNode	*prev;
	GreasePencilLayerTreeGroup	*parent;
	char	*name;
	int8_t	type;
	char	_pad[7];
	float	color[3];
	int	flag;
};

struct GreasePencilLayer	{
	GreasePencilLayerTreeNode	base;
	GreasePencilLayerFramesMapStorage	frames_storage;
	int8_t	blend_mode;
	char	_pad[3];
	float	opacity;
	ListBase	masks;
	int	active_mask_index;
	char	_pad2[4];
	Object	*parent;
	char	*parsubstr;
	float	parentinv[4][4];
	float	translation[3];
	float	rotation[3];
	float	scale[3];
	char	_pad3[4];
	char	*viewlayername;
	void	*runtime;
};

struct GreasePencilLayerTreeGroup	{
	GreasePencilLayerTreeNode	base;
	ListBase	children;
	int8_t	color_tag;
	char	_pad[7];
	void	*runtime;
};

struct GreasePencilOnionSkinningSettings	{
	float	opacity;
	int8_t	mode;
	uchar	flag;
	uchar	filter;
	char	_pad[1];
	short	num_frames_before;
	short	num_frames_after;
	float	color_before[3];
	float	color_after[3];
	char	_pad2[4];
};

struct GreasePencil	{
	ID	id;
	AnimData	*adt;
	GreasePencilDrawingBase	**drawing_array;
	int	drawing_array_size;
	char	_pad[4];
	GreasePencilLayerTreeGroup	*root_group_ptr;
	CustomData	layers_data;
	AttributeStorage	attribute_storage;
	int	attributes_active_index;
	char	_pad2[4];
	GreasePencilLayerTreeNode	*active_node;
	Material	**material_array;
	short	material_array_size;
	char	_pad3[2];
	int	flag;
	ListBase	vertex_group_names;
	int	vertex_group_active_index;
	char	_pad4[4];
	GreasePencilOnionSkinningSettings	onion_skinning_settings;
	void	*runtime;
};

struct ImageAnim	{
	ImageAnim	*next;
	ImageAnim	*prev;
	void	*anim;
};

struct ImageView	{
	ImageView	*next;
	ImageView	*prev;
	char	name[64];
	char	filepath[1024];
};

struct ImagePackedFile	{
	ImagePackedFile	*next;
	ImagePackedFile	*prev;
	PackedFile	*packedfile;
	int	view;
	int	tile_number;
	char	filepath[1024];
};

struct RenderSlot	{
	RenderSlot	*next;
	RenderSlot	*prev;
	char	name[64];
	void	*render;
};

struct ImageTile_Runtime	{
	int	tilearray_layer;
	int	_pad;
	int	tilearray_offset[2];
	int	tilearray_size[2];
};

struct ImageTile	{
	ImageTile	*next;
	ImageTile	*prev;
	ImageTile_Runtime	runtime;
	int	tile_number;
	int	gen_x;
	int	gen_y;
	char	gen_type;
	char	gen_flag;
	short	gen_depth;
	float	gen_color[4];
	char	label[64];
};

struct Stereo3dFormat;
struct Image	{
	ID	id;
	AnimData	*adt;
	char	name[1024];
	void	*cache;
	void	*gputexture[3][2];
	ListBase	anims;
	void	*rr;
	ListBase	renderslots;
	short	render_slot;
	short	last_render_slot;
	int	flag;
	short	source;
	short	type;
	int	lastframe;
	int	gpuframenr;
	short	gpuflag;
	short	gpu_pass;
	short	gpu_layer;
	short	gpu_view;
	short	seam_margin;
	char	_pad2[2];
	PackedFile	*packedfile;
	ListBase	packedfiles;
	PreviewImage	*preview;
	int	lastused;
	int	gen_x;
	int	gen_y;
	char	gen_type;
	char	gen_flag;
	short	gen_depth;
	float	gen_color[4];
	float	aspx;
	float	aspy;
	ColorManagedColorspaceSettings	colorspace_settings;
	char	alpha_mode;
	char	_pad;
	char	eye;
	char	views_format;
	int	active_tile_index;
	ListBase	tiles;
	ListBase	views;
	Stereo3dFormat	*stereo3d_format;
	void	*runtime;
};

struct IpoDriver	{
	Object	*ob;
	short	blocktype;
	short	adrcode;
	short	type;
	short	flag;
	char	name[128];
};

struct IpoCurve	{
	IpoCurve	*next;
	IpoCurve	*prev;
	BPoint	*bp;
	BezTriple	*bezt;
	rctf	maxrct;
	rctf	totrct;
	short	blocktype;
	short	adrcode;
	short	vartype;
	short	totvert;
	short	ipo;
	short	extrap;
	short	flag;
	char	_pad0[2];
	float	ymin;
	float	ymax;
	int	bitmask;
	float	slide_min;
	float	slide_max;
	float	curval;
	IpoDriver	*driver;
};

struct Ipo	{
	ID	id;
	ListBase	curve;
	rctf	cur;
	short	blocktype;
	short	showkey;
	short	muteipo;
	char	_pad[2];
};

struct KeyBlock	{
	KeyBlock	*next;
	KeyBlock	*prev;
	float	pos;
	float	curval;
	short	type;
	char	_pad1[2];
	short	relative;
	short	flag;
	int	totelem;
	int	uid;
	void	*data;
	char	name[64];
	char	vgroup[64];
	float	slidermin;
	float	slidermax;
};

struct Key	{
	ID	id;
	AnimData	*adt;
	KeyBlock	*refkey;
	char	elemstr[32];
	int	elemsize;
	char	_pad[4];
	ListBase	block;
	Ipo	*ipo;
	ID	*from;
	int	totkey;
	short	flag;
	char	type;
	char	_pad2;
	float	ctime;
	int	uidgen;
};

struct Lattice	{
	ID	id;
	AnimData	*adt;
	short	pntsu;
	short	pntsv;
	short	pntsw;
	short	flag;
	short	opntsu;
	short	opntsv;
	short	opntsw;
	char	_pad2[3];
	char	typeu;
	char	typev;
	char	typew;
	int	actbp;
	float	fu;
	float	fv;
	float	fw;
	float	du;
	float	dv;
	float	dw;
	BPoint	*def;
	Ipo	*ipo;
	Key	*key;
	MDeformVert	*dvert;
	char	vgroup[64];
	ListBase	vertex_group_names;
	int	vertex_group_active_index;
	char	_pad0[4];
	void	*editlatt;
	void	*batch_cache;
};

struct Base	{
	Base	*next;
	Base	*prev;
	Object	*object;
	Base	*base_orig;
	int	lay;
	short	flag;
	short	flag_from_collection;
	short	flag_legacy;
	short	local_view_bits;
	short	local_collections_bits;
	char	_pad1[2];
};

struct LayerCollection	{
	LayerCollection	*next;
	LayerCollection	*prev;
	Collection	*collection;
	void	*_pad1;
	short	flag;
	short	runtime_flag;
	char	_pad[4];
	ListBase	layer_collections;
	short	local_collections_bits;
	short	_pad2[3];
};

struct ViewLayerEEVEE	{
	int	render_passes;
	int	_pad[1];
};

struct ViewLayerAOV	{
	ViewLayerAOV	*next;
	ViewLayerAOV	*prev;
	char	name[64];
	int	flag;
	int	type;
};

struct ViewLayerLightgroup	{
	ViewLayerLightgroup	*next;
	ViewLayerLightgroup	*prev;
	char	name[64];
};

struct LightgroupMembership	{
	char	name[64];
};

struct World;
struct ViewLayer	{
	ViewLayer	*next;
	ViewLayer	*prev;
	char	name[64];
	short	flag;
	char	_pad[6];
	ListBase	object_bases;
	void	*stats;
	Base	*basact;
	ListBase	layer_collections;
	LayerCollection	*active_collection;
	int	layflag;
	int	passflag;
	float	pass_alpha_threshold;
	short	cryptomatte_flag;
	short	cryptomatte_levels;
	int	grease_pencil_flags;
	int	samples;
	Material	*mat_override;
	World	*world_override;
	IDProperty	*id_properties;
	IDProperty	*system_properties;
	FreestyleConfig	freestyle_config;
	ViewLayerEEVEE	eevee;
	ListBase	aovs;
	ViewLayerAOV	*active_aov;
	ListBase	lightgroups;
	ViewLayerLightgroup	*active_lightgroup;
	Base	**object_bases_array;
	void	*object_bases_hash;
};

struct bNodeTree;
struct Lamp	{
	ID	id;
	AnimData	*adt;
	short	type;
	short	flag;
	int	mode;
	float	r;
	float	g;
	float	b;
	float	temperature;
	float	energy_new;
	float	exposure;
	float	radius;
	float	spotsize;
	float	spotblend;
	short	area_shape;
	short	_pad1;
	float	area_size;
	float	area_sizey;
	float	area_sizez;
	float	area_spread;
	float	sun_angle;
	short	pr_texture;
	short	use_nodes;
	float	clipsta;
	float	clipend;
	float	cascade_max_dist;
	float	cascade_exponent;
	float	cascade_fade;
	int	cascade_count;
	float	diff_fac;
	float	spec_fac;
	float	transmission_fac;
	float	volume_fac;
	float	att_dist;
	float	shadow_filter_radius;
	float	shadow_maximum_resolution;
	float	shadow_jitter_overblur;
	PreviewImage	*preview;
	bNodeTree	*nodetree;
	Ipo	*ipo;
	float	energy;
	float	_pad2;
};

struct LightProbe	{
	ID	id;
	AnimData	*adt;
	char	type;
	char	flag;
	char	attenuation_type;
	char	parallax_type;
	char	grid_flag;
	char	_pad0[3];
	float	distinf;
	float	distpar;
	float	falloff;
	float	clipsta;
	float	clipend;
	float	vis_bias;
	float	vis_bleedbias;
	float	vis_blur;
	float	intensity;
	int	grid_resolution_x;
	int	grid_resolution_y;
	int	grid_resolution_z;
	int	grid_bake_samples;
	float	grid_surface_bias;
	float	grid_escape_bias;
	float	grid_normal_bias;
	float	grid_view_bias;
	float	grid_facing_bias;
	float	grid_validity_threshold;
	float	grid_dilation_threshold;
	float	grid_dilation_radius;
	float	grid_clamp_direct;
	float	grid_clamp_indirect;
	int	grid_surfel_density;
	Collection	*visibility_grp;
	float	data_display_size;
	char	_pad1[4];
};

struct LightProbeCache	{
	float	position[3];
	float	parallax_type;
	float	attenuation_fac;
	float	attenuation_type;
	float	_pad3[2];
	float	attenuationmat[4][4];
	float	parallaxmat[4][4];
};

struct LightGridCache	{
	float	mat[4][4];
	int	resolution[3];
	int	offset;
	float	corner[3];
	float	attenuation_scale;
	float	increment_x[3];
	float	attenuation_bias;
	float	increment_y[3];
	float	level_bias;
	float	increment_z[3];
	float	_pad4;
	float	visibility_bias;
	float	visibility_bleed;
	float	visibility_range;
	float	_pad5;
};

struct LightCacheTexture	{
	void	*tex;
	char	*data;
	int	tex_size[3];
	char	data_type;
	char	components;
	char	_pad[2];
};

struct LightCache	{
	int	flag;
	int	version;
	int	type;
	int	cube_len;
	int	grid_len;
	int	mips_len;
	int	vis_res;
	int	ref_res;
	char	_pad[4][2];
	LightCacheTexture	grid_tx;
	LightCacheTexture	cube_tx;
	LightCacheTexture	*cube_mips;
	LightProbeCache	*cube_data;
	LightGridCache	*grid_data;
};

struct LightProbeBakingData	{
	float	(*L0)();
	float	(*L1_a)();
	float	(*L1_b)();
	float	(*L1_c)();
	float	*validity;
	float	(*virtual_offset)();
};

struct LightProbeIrradianceData	{
	float	(*L0)();
	float	(*L1_a)();
	float	(*L1_b)();
	float	(*L1_c)();
};

struct LightProbeVisibilityData	{
	float	*L0;
	float	*L1_a;
	float	*L1_b;
	float	*L1_c;
};

struct LightProbeConnectivityData	{
	uchar	*validity;
};

struct LightProbeBlockData	{
	int	offset[3];
	int	level;
};

struct LightProbeGridCacheFrame	{
	int	size[3];
	int	data_layout;
	int	block_len;
	int	block_size;
	LightProbeBlockData	*block_infos;
	LightProbeBakingData	baking;
	LightProbeIrradianceData	irradiance;
	LightProbeVisibilityData	visibility;
	LightProbeConnectivityData	connectivity;
	char	_pad[4];
	int	surfels_len;
	void	*surfels;
};

struct LightProbeObjectCache	{
	int	cache_type;
	char	shared;
	char	dirty;
	char	_pad0[2];
	LightProbeGridCacheFrame	*grid_static_cache;
};

struct LineStyleModifier	{
	LineStyleModifier	*next;
	LineStyleModifier	*prev;
	char	name[64];
	int	type;
	float	influence;
	int	flags;
	int	blend;
};

struct LineStyleColorModifier_AlongStroke	{
	LineStyleModifier	modifier;
	ColorBand	*color_ramp;
};

struct LineStyleAlphaModifier_AlongStroke	{
	LineStyleModifier	modifier;
	CurveMapping	*curve;
	int	flags;
	char	_pad[4];
};

struct LineStyleThicknessModifier_AlongStroke	{
	LineStyleModifier	modifier;
	CurveMapping	*curve;
	int	flags;
	float	value_min;
	float	value_max;
	char	_pad[4];
};

struct LineStyleColorModifier_DistanceFromCamera	{
	LineStyleModifier	modifier;
	ColorBand	*color_ramp;
	float	range_min;
	float	range_max;
};

struct LineStyleAlphaModifier_DistanceFromCamera	{
	LineStyleModifier	modifier;
	CurveMapping	*curve;
	int	flags;
	float	range_min;
	float	range_max;
	char	_pad[4];
};

struct LineStyleThicknessModifier_DistanceFromCamera	{
	LineStyleModifier	modifier;
	CurveMapping	*curve;
	int	flags;
	float	range_min;
	float	range_max;
	float	value_min;
	float	value_max;
	char	_pad[4];
};

struct LineStyleColorModifier_DistanceFromObject	{
	LineStyleModifier	modifier;
	Object	*target;
	ColorBand	*color_ramp;
	float	range_min;
	float	range_max;
};

struct LineStyleAlphaModifier_DistanceFromObject	{
	LineStyleModifier	modifier;
	Object	*target;
	CurveMapping	*curve;
	int	flags;
	float	range_min;
	float	range_max;
	char	_pad[4];
};

struct LineStyleThicknessModifier_DistanceFromObject	{
	LineStyleModifier	modifier;
	Object	*target;
	CurveMapping	*curve;
	int	flags;
	float	range_min;
	float	range_max;
	float	value_min;
	float	value_max;
	char	_pad[4];
};

struct LineStyleColorModifier_Curvature_3D	{
	LineStyleModifier	modifier;
	float	min_curvature;
	float	max_curvature;
	ColorBand	*color_ramp;
	float	range_min;
	float	range_max;
};

struct LineStyleAlphaModifier_Curvature_3D	{
	LineStyleModifier	modifier;
	CurveMapping	*curve;
	int	flags;
	float	min_curvature;
	float	max_curvature;
	char	_pad[4];
};

struct LineStyleThicknessModifier_Curvature_3D	{
	LineStyleModifier	modifier;
	CurveMapping	*curve;
	int	flags;
	char	_pad[4];
	float	min_curvature;
	float	max_curvature;
	float	min_thickness;
	float	max_thickness;
};

struct LineStyleColorModifier_Noise	{
	LineStyleModifier	modifier;
	ColorBand	*color_ramp;
	float	period;
	float	amplitude;
	int	seed;
	char	_pad[4];
};

struct LineStyleAlphaModifier_Noise	{
	LineStyleModifier	modifier;
	CurveMapping	*curve;
	int	flags;
	float	period;
	float	amplitude;
	int	seed;
};

struct LineStyleThicknessModifier_Noise	{
	LineStyleModifier	modifier;
	float	period;
	float	amplitude;
	int	flags;
	int	seed;
};

struct LineStyleColorModifier_CreaseAngle	{
	LineStyleModifier	modifier;
	ColorBand	*color_ramp;
	float	min_angle;
	float	max_angle;
};

struct LineStyleAlphaModifier_CreaseAngle	{
	LineStyleModifier	modifier;
	CurveMapping	*curve;
	int	flags;
	float	min_angle;
	float	max_angle;
	char	_pad[4];
};

struct LineStyleThicknessModifier_CreaseAngle	{
	LineStyleModifier	modifier;
	CurveMapping	*curve;
	int	flags;
	char	_pad[4];
	float	min_angle;
	float	max_angle;
	float	min_thickness;
	float	max_thickness;
};

struct LineStyleColorModifier_Tangent	{
	LineStyleModifier	modifier;
	ColorBand	*color_ramp;
};

struct LineStyleAlphaModifier_Tangent	{
	LineStyleModifier	modifier;
	CurveMapping	*curve;
	int	flags;
	char	_pad[4];
};

struct LineStyleThicknessModifier_Tangent	{
	LineStyleModifier	modifier;
	CurveMapping	*curve;
	int	flags;
	float	min_thickness;
	float	max_thickness;
	char	_pad[4];
};

struct LineStyleColorModifier_Material	{
	LineStyleModifier	modifier;
	ColorBand	*color_ramp;
	int	flags;
	int	mat_attr;
};

struct LineStyleAlphaModifier_Material	{
	LineStyleModifier	modifier;
	CurveMapping	*curve;
	int	flags;
	int	mat_attr;
};

struct LineStyleThicknessModifier_Material	{
	LineStyleModifier	modifier;
	CurveMapping	*curve;
	int	flags;
	float	value_min;
	float	value_max;
	int	mat_attr;
};

struct LineStyleGeometryModifier_Sampling	{
	LineStyleModifier	modifier;
	float	sampling;
	char	_pad[4];
};

struct LineStyleGeometryModifier_BezierCurve	{
	LineStyleModifier	modifier;
	float	error;
	char	_pad[4];
};

struct LineStyleGeometryModifier_SinusDisplacement	{
	LineStyleModifier	modifier;
	float	wavelength;
	float	amplitude;
	float	phase;
	char	_pad[4];
};

struct LineStyleGeometryModifier_SpatialNoise	{
	LineStyleModifier	modifier;
	float	amplitude;
	float	scale;
	int	octaves;
	int	flags;
};

struct LineStyleGeometryModifier_PerlinNoise1D	{
	LineStyleModifier	modifier;
	float	frequency;
	float	amplitude;
	float	angle;
	int	octaves;
	int	seed;
	char	_pad1[4];
};

struct LineStyleGeometryModifier_PerlinNoise2D	{
	LineStyleModifier	modifier;
	float	frequency;
	float	amplitude;
	float	angle;
	int	octaves;
	int	seed;
	char	_pad1[4];
};

struct LineStyleGeometryModifier_BackboneStretcher	{
	LineStyleModifier	modifier;
	float	backbone_length;
	char	_pad[4];
};

struct LineStyleGeometryModifier_TipRemover	{
	LineStyleModifier	modifier;
	float	tip_length;
	char	_pad[4];
};

struct LineStyleGeometryModifier_Polygonalization	{
	LineStyleModifier	modifier;
	float	error;
	char	_pad[4];
};

struct LineStyleGeometryModifier_GuidingLines	{
	LineStyleModifier	modifier;
	float	offset;
	char	_pad[4];
};

struct LineStyleGeometryModifier_Blueprint	{
	LineStyleModifier	modifier;
	int	flags;
	int	rounds;
	float	backbone_length;
	int	random_radius;
	int	random_center;
	int	random_backbone;
};

struct LineStyleGeometryModifier_2DOffset	{
	LineStyleModifier	modifier;
	float	start;
	float	end;
	float	x;
	float	y;
};

struct LineStyleGeometryModifier_2DTransform	{
	LineStyleModifier	modifier;
	int	pivot;
	float	scale_x;
	float	scale_y;
	float	angle;
	float	pivot_u;
	float	pivot_x;
	float	pivot_y;
	char	_pad[4];
};

struct LineStyleGeometryModifier_Simplification	{
	LineStyleModifier	modifier;
	float	tolerance;
	char	_pad[4];
};

struct LineStyleThicknessModifier_Calligraphy	{
	LineStyleModifier	modifier;
	float	min_thickness;
	float	max_thickness;
	float	orientation;
	char	_pad[4];
};

struct FreestyleLineStyle	{
	ID	id;
	AnimData	*adt;
	float	r;
	float	g;
	float	b;
	float	alpha;
	float	thickness;
	int	thickness_position;
	float	thickness_ratio;
	int	flag;
	int	caps;
	int	chaining;
	int	rounds;
	float	split_length;
	float	min_angle;
	float	max_angle;
	float	min_length;
	float	max_length;
	int	chain_count;
	short	split_dash1;
	short	split_gap1;
	short	split_dash2;
	short	split_gap2;
	short	split_dash3;
	short	split_gap3;
	int	sort_key;
	int	integration_type;
	float	texstep;
	short	texact;
	short	pr_texture;
	short	use_nodes;
	char	_pad[6];
	short	dash1;
	short	gap1;
	short	dash2;
	short	gap2;
	short	dash3;
	short	gap3;
	int	panel;
	MTex	*mtex[18];
	bNodeTree	*nodetree;
	ListBase	color_modifiers;
	ListBase	alpha_modifiers;
	ListBase	thickness_modifiers;
	ListBase	geometry_modifiers;
};

struct Link	{
	Link	*next;
	Link	*prev;
};

struct LinkData	{
	LinkData	*next;
	LinkData	*prev;
	void	*data;
};

struct Mask_Runtime	{
	uint64_t	last_update;
};

struct Mask	{
	ID	id;
	AnimData	*adt;
	ListBase	masklayers;
	int	masklay_act;
	int	masklay_tot;
	int	sfra;
	int	efra;
	int	flag;
	char	_pad[4];
	void	*_pad1;
	Mask_Runtime	runtime;
};

struct MaskParent	{
	int	id_type;
	int	type;
	ID	*id;
	char	parent[64];
	char	sub_parent[64];
	float	parent_orig[2];
	float	parent_corners_orig[4][2];
};

struct MaskSplinePointUW	{
	float	u;
	float	w;
	int	flag;
};

struct MaskSplinePoint	{
	BezTriple	bezt;
	char	_pad[4];
	int	tot_uw;
	MaskSplinePointUW	*uw;
	MaskParent	parent;
};

struct MaskSpline	{
	MaskSpline	*next;
	MaskSpline	*prev;
	short	flag;
	char	offset_mode;
	char	weight_interp;
	int	tot_point;
	MaskSplinePoint	*points;
	MaskParent	parent;
	MaskSplinePoint	*points_deform;
};

struct MaskLayerShape	{
	MaskLayerShape	*next;
	MaskLayerShape	*prev;
	float	*data;
	int	tot_vert;
	int	frame;
	char	flag;
	char	_pad[7];
};

struct MaskLayer	{
	MaskLayer	*next;
	MaskLayer	*prev;
	char	name[64];
	ListBase	splines;
	ListBase	splines_shapes;
	MaskSpline	*act_spline;
	MaskSplinePoint	*act_point;
	float	alpha;
	char	blend;
	char	blend_flag;
	char	falloff;
	char	_pad[7];
	char	flag;
	char	restrictflag;
};

struct TexPaintSlot	{
	Image	*ima;
	ImageUser	*image_user;
	char	*uvname;
	char	*attribute_name;
	int	valid;
	int	interp;
};

struct MaterialGPencilStyle	{
	Image	*sima;
	Image	*ima;
	float	stroke_rgba[4];
	float	fill_rgba[4];
	float	mix_rgba[4];
	short	flag;
	short	index;
	short	stroke_style;
	short	fill_style;
	float	mix_factor;
	float	gradient_angle;
	float	gradient_radius;
	char	_pad2[4];
	float	gradient_scale[2];
	float	gradient_shift[2];
	float	texture_angle;
	float	texture_scale[2];
	float	texture_offset[2];
	float	texture_opacity;
	float	texture_pixsize;
	int	mode;
	int	gradient_type;
	float	mix_stroke_factor;
	int	alignment_mode;
	float	alignment_rotation;
};

struct MaterialLineArt	{
	int	flags;
	char	transparency_mask;
	char	mat_occlusion;
	char	intersection_priority;
	char	_pad;
};

struct Material	{
	ID	id;
	AnimData	*adt;
	short	flag;
	char	surface_render_method;
	char	_pad1[1];
	float	r;
	float	g;
	float	b;
	float	a;
	float	specr;
	float	specg;
	float	specb;
	float	alpha;
	float	ray_mirror;
	float	spec;
	float	gloss_mir;
	float	roughness;
	float	metallic;
	char	use_nodes;
	char	pr_type;
	short	pr_texture;
	short	pr_flag;
	short	index;
	bNodeTree	*nodetree;
	Ipo	*ipo;
	PreviewImage	*preview;
	float	line_col[4];
	short	line_priority;
	short	vcol_alpha;
	short	paint_active_slot;
	short	paint_clone_slot;
	short	tot_slots;
	char	displacement_method;
	char	thickness_mode;
	float	alpha_threshold;
	float	refract_depth;
	char	blend_method;
	char	blend_shadow;
	char	blend_flag;
	char	volume_intersection_method;
	float	inflate_bounds;
	char	_pad3[4];
	TexPaintSlot	*texpaintslot;
	ListBase	gpumaterial;
	MaterialGPencilStyle	*gp_style;
	MaterialLineArt	lineart;
};

struct MSelect;
struct MPoly;
struct MLoop;
struct MVert;
struct MEdge;
struct MTFace;
struct TFace;
struct MCol;
struct MFace;
struct Mesh	{
	ID	id;
	AnimData	*adt;
	Ipo	*ipo;
	Key	*key;
	Material	**mat;
	int	totvert;
	int	totedge;
	int	totpoly;
	int	totloop;
	int	*poly_offset_indices;
	AttributeStorage	attribute_storage;
	CustomData	vdata;
	CustomData	edata;
	CustomData	pdata;
	CustomData	ldata;
	ListBase	vertex_group_names;
	int	vertex_group_active_index;
	int	attributes_active_index;
	MSelect	*mselect;
	int	totselect;
	int	act_face;
	Mesh	*texcomesh;
	float	loc[3];
	float	size[3];
	char	texflag;
	char	editflag;
	ushort	flag;
	float	smoothresh;
	float	remesh_voxel_size;
	float	remesh_voxel_adaptivity;
	int	face_sets_color_seed;
	int	face_sets_color_default;
	char	*active_color_attribute;
	char	*default_color_attribute;
	char	*active_uv_map_attribute;
	char	*default_uv_map_attribute;
	char	symmetry;
	char	remesh_mode;
	short	totcol;
	char	cd_flag;
	char	subdiv;
	char	subdivr;
	char	subsurftype;
	MPoly	*mpoly;
	MLoop	*mloop;
	MVert	*mvert;
	MEdge	*medge;
	MDeformVert	*dvert;
	MTFace	*mtface;
	TFace	*tface;
	MCol	*mcol;
	MFace	*mface;
	CustomData	fdata;
	int	totface;
	char	_pad1[4];
	void	*runtime;
};

struct TFace	{
	void	*tpage;
	float	uv[4][2];
	int	col[4];
	char	flag;
	char	transp;
	short	mode;
	short	tile;
	short	unwrap;
};

struct MSelect	{
	int	index;
	int	type;
};

struct MFloatProperty	{
	float	f;
};

struct MIntProperty	{
	int	i;
};

struct MStringProperty	{
	char	s[255];
	char	s_len;
};

struct MBoolProperty	{
	uchar	b;
};

struct MInt8Property	{
	int8_t	i;
};

struct MDeformWeight	{
	int	def_nr;
	float	weight;
};

struct MDeformVert	{
	MDeformWeight	*dw;
	int	totweight;
	int	flag;
};

struct MVertSkin	{
	float	radius[3];
	int	flag;
};

struct MLoopCol	{
	char	r;
	char	g;
	char	b;
	char	a;
};

struct MPropCol	{
	float	color[4];
};

struct MDisps	{
	int	totdisp;
	int	level;
	float	(*disps)();
	int	*hidden;
};

struct GridPaintMask	{
	float	*data;
	int	level;
	char	_pad[4];
};

struct FreestyleEdge	{
	char	flag;
};

struct FreestyleFace	{
	char	flag;
};

struct MEdge	{
	int	v1;
	int	v2;
	char	crease;
	char	bweight;
	short	flag;
};

struct MPoly	{
	int	loopstart;
	int	totloop;
	short	mat_nr;
	char	flag;
	char	_pad;
};

struct MLoopUV	{
	float	uv[2];
	int	flag;
};

struct MVert	{
	float	co[3];
	char	flag;
	char	bweight;
	char	_pad[2];
};

struct MLoop	{
	int	v;
	int	e;
};

struct MFace	{
	int	v1;
	int	v2;
	int	v3;
	int	v4;
	short	mat_nr;
	char	edcode;
	char	flag;
};

struct MTFace	{
	float	uv[4][2];
};

struct MCol	{
	char	a;
	char	r;
	char	g;
	char	b;
};

struct MRecast	{
	int	i;
};

struct BoundBox;
struct MetaElem	{
	MetaElem	*next;
	MetaElem	*prev;
	BoundBox	*bb;
	short	type;
	short	flag;
	char	_pad[4];
	float	x;
	float	y;
	float	z;
	float	quat[4];
	float	expx;
	float	expy;
	float	expz;
	float	rad;
	float	rad2;
	float	s;
	float	len;
	float	*mat;
	float	*imat;
};

struct MetaBall	{
	ID	id;
	AnimData	*adt;
	ListBase	elems;
	ListBase	*editelems;
	Ipo	*ipo;
	Material	**mat;
	char	flag;
	char	flag2;
	short	totcol;
	char	texflag;
	char	_pad[2];
	char	needs_flush_to_id;
	float	loc[3];
	float	size[3];
	float	wiresize;
	float	rendersize;
	float	thresh;
	char	_pad0[4];
	MetaElem	*lastelem;
};

struct ModifierData	{
	ModifierData	*next;
	ModifierData	*prev;
	int	type;
	int	mode;
	float	execution_time;
	short	flag;
	short	ui_expand_flag;
	ushort	layout_panel_open_flag;
	char	_pad[2];
	int	persistent_uid;
	char	name[64];
	char	*error;
	void	*runtime;
};

struct MappingInfoModifierData	{
	ModifierData	modifier;
	Tex	*texture;
	Object	*map_object;
	char	map_bone[64];
	char	uvlayer_name[68];
	char	_pad1[4];
	int	uvlayer_tmp;
	int	texmapping;
};

struct SubsurfModifierData	{
	ModifierData	modifier;
	short	subdivType;
	short	levels;
	short	renderLevels;
	short	flags;
	short	uv_smooth;
	short	quality;
	short	boundary_smooth;
	char	_pad[2];
	void	*emCache;
	void	*mCache;
};

struct LatticeModifierData	{
	ModifierData	modifier;
	Object	*object;
	char	name[64];
	float	strength;
	short	flag;
	char	_pad[2];
	void	*_pad1;
};

struct CurveModifierData	{
	ModifierData	modifier;
	Object	*object;
	char	name[64];
	short	defaxis;
	short	flag;
	char	_pad[4];
	void	*_pad1;
};

struct BuildModifierData	{
	ModifierData	modifier;
	float	start;
	float	length;
	short	flag;
	short	randomize;
	int	seed;
};

struct MaskModifierData	{
	ModifierData	modifier;
	Object	*ob_arm;
	char	vgroup[64];
	short	mode;
	short	flag;
	float	threshold;
	void	*_pad1;
};

struct ArrayModifierData	{
	ModifierData	modifier;
	Object	*start_cap;
	Object	*end_cap;
	Object	*curve_ob;
	Object	*offset_ob;
	float	offset[3];
	float	scale[3];
	float	length;
	float	merge_dist;
	int	fit_type;
	int	offset_type;
	int	flags;
	int	count;
	float	uv_offset[2];
};

struct MirrorModifierData	{
	ModifierData	modifier;
	short	axis;
	short	flag;
	float	tolerance;
	float	bisect_threshold;
	uchar	use_correct_order_on_merge;
	char	_pad[3];
	float	uv_offset[2];
	float	uv_offset_copy[2];
	Object	*mirror_ob;
	void	*_pad1;
};

struct EdgeSplitModifierData	{
	ModifierData	modifier;
	float	split_angle;
	int	flags;
};

struct BevelModifierData	{
	ModifierData	modifier;
	float	value;
	int	res;
	short	flags;
	short	val_flags;
	short	profile_type;
	short	lim_flags;
	short	e_flags;
	short	mat;
	short	edge_flags;
	short	face_str_mode;
	short	miter_inner;
	short	miter_outer;
	short	vmesh_method;
	char	affect_type;
	char	_pad;
	float	profile;
	float	bevel_angle;
	float	spread;
	char	defgrp_name[64];
	char	_pad1[4];
	CurveProfile	*custom_profile;
	char	edge_weight_name[64];
	char	vertex_weight_name[64];
};

struct FluidModifierData	{
	ModifierData	modifier;
	FluidDomainSettings	*domain;
	FluidFlowSettings	*flow;
	FluidEffectorSettings	*effector;
	float	time;
	int	type;
	void	*_pad1;
};

struct DisplaceModifierData	{
	ModifierData	modifier;
	Tex	*texture;
	Object	*map_object;
	char	map_bone[64];
	char	uvlayer_name[68];
	char	_pad1[4];
	int	uvlayer_tmp;
	int	texmapping;
	float	strength;
	int	direction;
	char	defgrp_name[64];
	float	midlevel;
	int	space;
	short	flag;
	char	_pad2[6];
};

struct UVProjectModifierData	{
	ModifierData	modifier;
	Object	*projectors[10];
	char	_pad2[4];
	int	num_projectors;
	float	aspectx;
	float	aspecty;
	float	scalex;
	float	scaley;
	char	uvlayer_name[68];
	int	uvlayer_tmp;
};

struct DecimateModifierData	{
	ModifierData	modifier;
	float	percent;
	short	iter;
	char	delimit;
	char	symmetry_axis;
	float	angle;
	char	defgrp_name[64];
	float	defgrp_factor;
	short	flag;
	short	mode;
	int	face_count;
};

struct SmoothModifierData	{
	ModifierData	modifier;
	float	fac;
	char	defgrp_name[64];
	short	flag;
	short	repeat;
};

struct CastModifierData	{
	ModifierData	modifier;
	Object	*object;
	float	fac;
	float	radius;
	float	size;
	char	defgrp_name[64];
	short	flag;
	short	type;
	void	*_pad1;
};

struct WaveModifierData	{
	ModifierData	modifier;
	Tex	*texture;
	Object	*map_object;
	char	map_bone[64];
	char	uvlayer_name[68];
	char	_pad1[4];
	int	uvlayer_tmp;
	int	texmapping;
	Object	*objectcenter;
	char	defgrp_name[64];
	short	flag;
	char	_pad2[2];
	float	startx;
	float	starty;
	float	height;
	float	width;
	float	narrow;
	float	speed;
	float	damp;
	float	falloff;
	float	timeoffs;
	float	lifetime;
	char	_pad3[4];
	void	*_pad4;
};

struct ArmatureModifierData	{
	ModifierData	modifier;
	short	deformflag;
	short	multi;
	char	_pad2[4];
	Object	*object;
	float	(*vert_coords_prev)();
	char	defgrp_name[64];
};

struct HookModifierData	{
	ModifierData	modifier;
	Object	*object;
	char	subtarget[64];
	char	flag;
	char	falloff_type;
	char	_pad[6];
	float	parentinv[4][4];
	float	cent[3];
	float	falloff;
	CurveMapping	*curfalloff;
	int	*indexar;
	int	totindex;
	float	force;
	char	name[64];
	void	*_pad1;
};

struct SoftbodyModifierData	{
	ModifierData	modifier;
};

struct ClothModifierData	{
	ModifierData	modifier;
	void	*clothObject;
	ClothSimSettings	*sim_parms;
	ClothCollSettings	*coll_parms;
	PointCache	*point_cache;
	ListBase	ptcaches;
	void	*hairdata;
	float	hair_grid_min[3];
	float	hair_grid_max[3];
	int	hair_grid_res[3];
	float	hair_grid_cellsize;
	void	*solver_result;
};

struct CollisionModifierData	{
	ModifierData	modifier;
	float	(*x)();
	float	(*xnew)();
	float	(*xold)();
	float	(*current_xnew)();
	float	(*current_x)();
	float	(*current_v)();
	int	(*vert_tris)();
	int	mvert_num;
	int	tri_num;
	float	time_x;
	float	time_xnew;
	char	is_static;
	char	_pad[7];
	void	*bvhtree;
};

struct SurfaceModifierData_Runtime	{
	float	(*vert_positions_prev)();
	float	(*vert_velocities)();
	Mesh	*mesh;
	void	*bvhtree;
	int	cfra_prev;
	int	verts_num;
};

struct SurfaceModifierData	{
	ModifierData	modifier;
	SurfaceModifierData_Runtime	runtime;
};

struct BooleanModifierData	{
	ModifierData	modifier;
	Object	*object;
	Collection	*collection;
	float	double_threshold;
	char	operation;
	char	solver;
	char	material_mode;
	char	flag;
	char	bm_flag;
	char	_pad[7];
};

struct MDefInfluence	{
	int	vertex;
	float	weight;
};

struct MDefCell	{
	int	offset;
	int	totinfluence;
};

struct MeshDeformModifierData	{
	ModifierData	modifier;
	Object	*object;
	char	defgrp_name[64];
	short	gridsize;
	short	flag;
	char	_pad[4];
	MDefInfluence	*bindinfluences;
	int	*bindoffsets;
	float	*bindcagecos;
	int	totvert;
	int	totcagevert;
	MDefCell	*dyngrid;
	MDefInfluence	*dyninfluences;
	int	*dynverts;
	int	dyngridsize;
	int	totinfluence;
	float	dyncellmin[3];
	float	dyncellwidth;
	float	bindmat[4][4];
	float	*bindweights;
	float	*bindcos;
	void	(*bindfunc)();
};

struct ParticleSystemModifierData	{
	ModifierData	modifier;
	ParticleSystem	*psys;
	Mesh	*mesh_final;
	Mesh	*mesh_original;
	int	totdmvert;
	int	totdmedge;
	int	totdmface;
	short	flag;
	char	_pad[2];
	void	*_pad1;
};

struct ParticleInstanceModifierData	{
	ModifierData	modifier;
	Object	*ob;
	short	psys;
	short	flag;
	short	axis;
	short	space;
	float	position;
	float	random_position;
	float	rotation;
	float	random_rotation;
	float	particle_amount;
	float	particle_offset;
	char	index_layer_name[68];
	char	value_layer_name[68];
	void	*_pad1;
};

struct ExplodeModifierData	{
	ModifierData	modifier;
	int	*facepa;
	short	flag;
	short	vgroup;
	float	protect;
	char	uvname[68];
	char	_pad1[4];
	void	*_pad2;
};

struct MultiresModifierData	{
	ModifierData	modifier;
	char	lvl;
	char	sculptlvl;
	char	renderlvl;
	char	totlvl;
	char	simple;
	char	flags;
	char	_pad[2];
	short	quality;
	short	uv_smooth;
	short	boundary_smooth;
	char	_pad2[2];
};

struct FluidsimSettings;
struct FluidsimModifierData	{
	ModifierData	modifier;
	FluidsimSettings	*fss;
	void	*_pad1;
};

struct SmokeModifierData	{
	ModifierData	modifier;
	int	type;
	int	_pad;
};

struct ShrinkwrapModifierData	{
	ModifierData	modifier;
	Object	*target;
	Object	*auxTarget;
	char	vgroup_name[64];
	float	keepDist;
	short	shrinkType;
	char	shrinkOpts;
	char	shrinkMode;
	float	projLimit;
	char	projAxis;
	char	subsurfLevels;
	char	_pad[2];
};

struct SimpleDeformModifierData	{
	ModifierData	modifier;
	Object	*origin;
	char	vgroup_name[64];
	float	factor;
	float	limit[2];
	char	mode;
	char	axis;
	char	deform_axis;
	char	flag;
	void	*_pad1;
};

struct ShapeKeyModifierData	{
	ModifierData	modifier;
};

struct SolidifyModifierData	{
	ModifierData	modifier;
	char	defgrp_name[64];
	char	shell_defgrp_name[64];
	char	rim_defgrp_name[64];
	float	offset;
	float	offset_fac;
	float	offset_fac_vg;
	float	offset_clamp;
	char	mode;
	char	nonmanifold_offset_mode;
	char	nonmanifold_boundary_mode;
	char	_pad;
	float	crease_inner;
	float	crease_outer;
	float	crease_rim;
	int	flag;
	short	mat_ofs;
	short	mat_ofs_rim;
	float	merge_tolerance;
	float	bevel_convex;
};

struct ScrewModifierData	{
	ModifierData	modifier;
	Object	*ob_axis;
	int	steps;
	int	render_steps;
	int	iter;
	float	screw_ofs;
	float	angle;
	float	merge_dist;
	short	flag;
	char	axis;
	char	_pad[5];
	void	*_pad1;
};

struct OceanModifierData	{
	ModifierData	modifier;
	void	*ocean;
	void	*oceancache;
	int	resolution;
	int	viewport_resolution;
	int	spatial_size;
	float	wind_velocity;
	float	damp;
	float	smallest_wave;
	float	depth;
	float	wave_alignment;
	float	wave_direction;
	float	wave_scale;
	float	chop_amount;
	float	foam_coverage;
	float	time;
	int	spectrum;
	float	fetch_jonswap;
	float	sharpen_peak_jonswap;
	int	bakestart;
	int	bakeend;
	char	cachepath[1024];
	char	foamlayername[68];
	char	spraylayername[68];
	char	cached;
	char	geometry_mode;
	char	flag;
	char	_pad2;
	short	repeat_x;
	short	repeat_y;
	int	seed;
	float	size;
	float	foam_fade;
	char	_pad[4];
};

struct WarpModifierData	{
	ModifierData	modifier;
	Tex	*texture;
	Object	*map_object;
	char	map_bone[64];
	char	uvlayer_name[68];
	char	_pad1[4];
	int	uvlayer_tmp;
	int	texmapping;
	Object	*object_from;
	Object	*object_to;
	char	bone_from[64];
	char	bone_to[64];
	CurveMapping	*curfalloff;
	char	defgrp_name[64];
	float	strength;
	float	falloff_radius;
	char	flag;
	char	falloff_type;
	char	_pad2[6];
	void	*_pad3;
};

struct WeightVGEditModifierData	{
	ModifierData	modifier;
	char	defgrp_name[64];
	short	edit_flags;
	short	falloff_type;
	float	default_weight;
	CurveMapping	*cmap_curve;
	float	add_threshold;
	float	rem_threshold;
	float	mask_constant;
	char	mask_defgrp_name[64];
	int	mask_tex_use_channel;
	Tex	*mask_texture;
	Object	*mask_tex_map_obj;
	char	mask_tex_map_bone[64];
	int	mask_tex_mapping;
	char	mask_tex_uvlayer_name[68];
	void	*_pad1;
};

struct WeightVGMixModifierData	{
	ModifierData	modifier;
	char	defgrp_name_a[64];
	char	defgrp_name_b[64];
	float	default_weight_a;
	float	default_weight_b;
	char	mix_mode;
	char	mix_set;
	char	_pad0[6];
	float	mask_constant;
	char	mask_defgrp_name[64];
	int	mask_tex_use_channel;
	Tex	*mask_texture;
	Object	*mask_tex_map_obj;
	char	mask_tex_map_bone[64];
	int	mask_tex_mapping;
	char	mask_tex_uvlayer_name[68];
	char	_pad1[4];
	char	flag;
	char	_pad2[3];
};

struct WeightVGProximityModifierData	{
	ModifierData	modifier;
	char	defgrp_name[64];
	CurveMapping	*cmap_curve;
	int	proximity_mode;
	int	proximity_flags;
	Object	*proximity_ob_target;
	float	mask_constant;
	char	mask_defgrp_name[64];
	int	mask_tex_use_channel;
	Tex	*mask_texture;
	Object	*mask_tex_map_obj;
	char	mask_tex_map_bone[64];
	int	mask_tex_mapping;
	char	mask_tex_uvlayer_name[68];
	char	_pad1[4];
	float	min_dist;
	float	max_dist;
	short	falloff_type;
	char	_pad0[2];
};

struct DynamicPaintModifierData	{
	ModifierData	modifier;
	DynamicPaintCanvasSettings	*canvas;
	DynamicPaintBrushSettings	*brush;
	int	type;
	char	_pad[4];
};

struct RemeshModifierData	{
	ModifierData	modifier;
	float	threshold;
	float	scale;
	float	hermite_num;
	char	depth;
	char	flag;
	char	mode;
	char	_pad;
	float	voxel_size;
	float	adaptivity;
};

struct SkinModifierData	{
	ModifierData	modifier;
	float	branch_smoothing;
	char	flag;
	char	symmetry_axes;
	char	_pad[2];
};

struct TriangulateModifierData	{
	ModifierData	modifier;
	int	flag;
	int	quad_method;
	int	ngon_method;
	int	min_vertices;
};

struct LaplacianSmoothModifierData	{
	ModifierData	modifier;
	float	lambda;
	float	lambda_border;
	char	_pad1[4];
	char	defgrp_name[64];
	short	flag;
	short	repeat;
};

struct CorrectiveSmoothDeltaCache	{
	float	(*deltas)();
	int	deltas_num;
	float	lambda;
	float	scale;
	short	repeat;
	short	flag;
	char	smooth_type;
	char	rest_source;
	char	_pad[6];
};

struct CorrectiveSmoothModifierData	{
	ModifierData	modifier;
	float	(*bind_coords)();
	int	bind_coords_num;
	float	lambda;
	float	scale;
	short	repeat;
	short	flag;
	char	smooth_type;
	char	rest_source;
	char	_pad[6];
	char	defgrp_name[64];
	CorrectiveSmoothDeltaCache	delta_cache;
};

struct UVWarpModifierData	{
	ModifierData	modifier;
	char	axis_u;
	char	axis_v;
	short	flag;
	float	center[2];
	float	offset[2];
	float	scale[2];
	float	rotation;
	Object	*object_src;
	char	bone_src[64];
	Object	*object_dst;
	char	bone_dst[64];
	char	vgroup_name[64];
	char	uvlayer_name[68];
	char	_pad[4];
};

struct MeshCacheModifierData	{
	ModifierData	modifier;
	char	flag;
	char	type;
	char	time_mode;
	char	play_mode;
	char	forward_axis;
	char	up_axis;
	char	flip_axis;
	char	interp;
	float	factor;
	char	deform_mode;
	char	defgrp_name[64];
	char	_pad[7];
	float	frame_start;
	float	frame_scale;
	float	eval_frame;
	float	eval_time;
	float	eval_factor;
	char	filepath[1024];
};

struct LaplacianDeformModifierData	{
	ModifierData	modifier;
	char	anchor_grp_name[64];
	int	total_verts;
	int	repeat;
	float	*vertexco;
	void	*cache_system;
	short	flag;
	char	_pad[6];
};

struct WireframeModifierData	{
	ModifierData	modifier;
	char	defgrp_name[64];
	float	offset;
	float	offset_fac;
	float	offset_fac_vg;
	float	crease_weight;
	short	flag;
	short	mat_ofs;
	char	_pad[4];
};

struct WeldModifierData	{
	ModifierData	modifier;
	float	merge_dist;
	char	defgrp_name[64];
	char	mode;
	char	flag;
	char	_pad[2];
};

struct DataTransferModifierData	{
	ModifierData	modifier;
	Object	*ob_source;
	int	data_types;
	int	vmap_mode;
	int	emap_mode;
	int	lmap_mode;
	int	pmap_mode;
	float	map_max_distance;
	float	map_ray_radius;
	float	islands_precision;
	char	_pad1[4];
	int	layers_select_src[5];
	int	layers_select_dst[5];
	int	mix_mode;
	float	mix_factor;
	char	defgrp_name[64];
	int	flags;
	void	*_pad2;
};

struct NormalEditModifierData	{
	ModifierData	modifier;
	char	defgrp_name[64];
	Object	*target;
	short	mode;
	short	flag;
	short	mix_mode;
	char	_pad[2];
	float	mix_factor;
	float	mix_limit;
	float	offset[3];
	char	_pad0[4];
	void	*_pad1;
};

struct MeshSeqCacheModifierData	{
	ModifierData	modifier;
	CacheFile	*cache_file;
	char	object_path[1024];
	char	read_flag;
	char	_pad[3];
	float	velocity_scale;
	void	*reader;
	char	reader_object_path[1024];
};

struct SDefBind	{
	int	*vert_inds;
	int	numverts;
	int	mode;
	float	*vert_weights;
	float	normal_dist;
	float	influence;
};

struct SDefVert	{
	SDefBind	*binds;
	int	numbinds;
	int	vertex_idx;
};

struct SurfaceDeformModifierData	{
	ModifierData	modifier;
	void	*depsgraph;
	Object	*target;
	SDefVert	*verts;
	void	*_pad1;
	float	falloff;
	int	num_mesh_verts;
	int	numverts;
	int	target_verts_num;
	int	numpoly;
	int	flags;
	float	mat[4][4];
	float	strength;
	char	defgrp_name[64];
	int	_pad2;
};

struct WeightedNormalModifierData	{
	ModifierData	modifier;
	char	defgrp_name[64];
	char	mode;
	char	flag;
	short	weight;
	float	thresh;
};

struct NodesModifierSettings	{
	IDProperty	*properties;
};

struct NodesModifierDataBlock	{
	char	*id_name;
	char	*lib_name;
	ID	*id;
	int	id_type;
	char	_pad[4];
};

struct NodesModifierBakeFile	{
	char	*name;
	PackedFile	*packed_file;
};

struct NodesModifierPackedBake	{
	int	meta_files_num;
	int	blob_files_num;
	NodesModifierBakeFile	*meta_files;
	NodesModifierBakeFile	*blob_files;
};

struct NodesModifierBake	{
	int	id;
	int	flag;
	uchar	bake_mode;
	int8_t	bake_target;
	char	_pad[6];
	char	*directory;
	int	frame_start;
	int	frame_end;
	int	data_blocks_num;
	int	active_data_block;
	NodesModifierDataBlock	*data_blocks;
	NodesModifierPackedBake	*packed;
	void	*_pad2;
	int64_t	bake_size;
};

struct NodesModifierPanel	{
	int	id;
	int	flag;
};

struct NodesModifierData	{
	ModifierData	modifier;
	bNodeTree	*node_group;
	NodesModifierSettings	settings;
	char	*simulation_bake_directory;
	int8_t	flag;
	int8_t	bake_target;
	char	_pad[2];
	int	bakes_num;
	NodesModifierBake	*bakes;
	char	_pad2[4];
	int	panels_num;
	NodesModifierPanel	*panels;
	void	*runtime;
};

struct MeshToVolumeModifierData	{
	ModifierData	modifier;
	Object	*object;
	int	resolution_mode;
	float	voxel_size;
	int	voxel_amount;
	float	interior_band_width;
	float	density;
	char	_pad2[4];
	void	*_pad3;
};

struct VolumeDisplaceModifierData	{
	ModifierData	modifier;
	Tex	*texture;
	Object	*texture_map_object;
	int	texture_map_mode;
	float	strength;
	float	texture_mid_level[3];
	float	texture_sample_radius;
};

struct VolumeToMeshModifierData	{
	ModifierData	modifier;
	Object	*object;
	float	threshold;
	float	adaptivity;
	int	flag;
	int	resolution_mode;
	float	voxel_size;
	int	voxel_amount;
	char	grid_name[64];
	void	*_pad1;
};

struct GreasePencilModifierInfluenceData	{
	int	flag;
	char	_pad1[4];
	char	layer_name[64];
	Material	*material;
	int	layer_pass;
	int	material_pass;
	char	vertex_group_name[64];
	CurveMapping	*custom_curve;
	void	*_pad2;
};

struct GreasePencilOpacityModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	int	flag;
	char	color_mode;
	char	_pad1[3];
	float	color_factor;
	float	hardness_factor;
	void	*_pad2;
};

struct GreasePencilSubdivModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	int	type;
	int	level;
	char	_pad[8];
	void	*_pad1;
};

struct GreasePencilColorModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	char	color_mode;
	char	_pad1[3];
	float	hsv[3];
	void	*_pad2;
};

struct GreasePencilTintModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	short	flag;
	char	color_mode;
	char	tint_mode;
	float	factor;
	float	radius;
	float	color[3];
	Object	*object;
	ColorBand	*color_ramp;
	void	*_pad;
};

struct GreasePencilSmoothModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	int	flag;
	float	factor;
	int	step;
	char	_pad[4];
	void	*_pad1;
};

struct GreasePencilOffsetModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	int	flag;
	int	offset_mode;
	float	loc[3];
	float	rot[3];
	float	scale[3];
	float	stroke_loc[3];
	float	stroke_rot[3];
	float	stroke_scale[3];
	int	seed;
	int	stroke_step;
	int	stroke_start_offset;
	char	_pad1[4];
	void	*_pad2;
};

struct GreasePencilNoiseModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	int	flag;
	float	factor;
	float	factor_strength;
	float	factor_thickness;
	float	factor_uvs;
	float	noise_scale;
	float	noise_offset;
	short	noise_mode;
	char	_pad[2];
	int	step;
	int	seed;
	void	*_pad1;
};

struct GreasePencilMirrorModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	Object	*object;
	int	flag;
	char	_pad[4];
};

struct GreasePencilThickModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	int	flag;
	float	thickness_fac;
	float	thickness;
	char	_pad[4];
	void	*_pad1;
};

struct GreasePencilLatticeModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	Object	*object;
	float	strength;
	char	_pad[4];
};

struct GreasePencilDashModifierSegment	{
	char	name[64];
	int	dash;
	int	gap;
	float	radius;
	float	opacity;
	int	mat_nr;
	int	flag;
};

struct GreasePencilDashModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	GreasePencilDashModifierSegment	*segments_array;
	int	segments_num;
	int	segment_active_index;
	int	dash_offset;
	char	_pad[4];
};

struct GreasePencilMultiModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	int	flag;
	int	duplications;
	float	distance;
	float	offset;
	float	fading_center;
	float	fading_thickness;
	float	fading_opacity;
	int	_pad0;
	void	*_pad;
};

struct GreasePencilLengthModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	int	flag;
	float	start_fac;
	float	end_fac;
	float	rand_start_fac;
	float	rand_end_fac;
	float	rand_offset;
	float	overshoot_fac;
	int	seed;
	int	step;
	int	mode;
	char	_pad[4];
	float	point_density;
	float	segment_influence;
	float	max_angle;
	void	*_pad1;
};

struct GreasePencilWeightAngleModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	int	flag;
	float	min_weight;
	short	axis;
	short	space;
	float	angle;
	char	target_vgname[64];
	void	*_pad;
};

struct GreasePencilArrayModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	Object	*object;
	int	count;
	int	flag;
	float	offset[3];
	float	shift[3];
	float	rnd_offset[3];
	float	rnd_rot[3];
	float	rnd_scale[3];
	char	_pad[4];
	int	seed;
	int	mat_rpl;
};

struct GreasePencilWeightProximityModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	int	flag;
	char	target_vgname[64];
	float	min_weight;
	float	dist_start;
	float	dist_end;
	Object	*object;
};

struct GreasePencilHookModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	Object	*object;
	char	subtarget[64];
	char	_pad[4];
	int	flag;
	char	falloff_type;
	char	_pad1[3];
	float	parentinv[4][4];
	float	cent[3];
	float	falloff;
	float	force;
};

struct GreasePencilLineartModifierData	{
	ModifierData	modifier;
	ushort	edge_types;
	char	source_type;
	char	use_multiple_levels;
	short	level_start;
	short	level_end;
	Object	*source_camera;
	Object	*light_contour_object;
	Object	*source_object;
	Collection	*source_collection;
	Material	*target_material;
	char	target_layer[64];
	char	source_vertex_group[64];
	char	vgname[64];
	float	overscan;
	float	shadow_camera_fov;
	float	shadow_camera_size;
	float	shadow_camera_near;
	float	shadow_camera_far;
	float	opacity;
	short	thickness;
	char	mask_switches;
	char	material_mask_bits;
	char	intersection_mask;
	char	shadow_selection;
	char	silhouette_selection;
	char	_pad[1];
	float	crease_threshold;
	float	angle_splitting_threshold;
	float	chain_smooth_tolerance;
	float	chaining_image_threshold;
	int	calculation_flags;
	int	flags;
	float	stroke_depth_offset;
	char	level_start_override;
	char	level_end_override;
	short	edge_types_override;
	char	shadow_selection_override;
	char	shadow_use_silhouette_override;
	char	_pad2[6];
	void	*shared_cache;
	void	*cache;
	void	*la_data_ptr;
	void	*runtime;
};

struct GreasePencilArmatureModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	Object	*object;
	short	deformflag;
	char	_pad[6];
};

struct GreasePencilTimeModifierSegment	{
	char	name[64];
	int	segment_start;
	int	segment_end;
	int	segment_mode;
	int	segment_repeat;
};

struct GreasePencilTimeModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	int	flag;
	int	offset;
	float	frame_scale;
	int	mode;
	int	sfra;
	int	efra;
	GreasePencilTimeModifierSegment	*segments_array;
	int	segments_num;
	int	segment_active_index;
};

struct GreasePencilEnvelopeModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	int	mode;
	int	mat_nr;
	float	thickness;
	float	strength;
	int	skip;
	int	spread;
};

struct GreasePencilOutlineModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	Object	*object;
	int	flag;
	int	thickness;
	float	sample_length;
	int	subdiv;
	Material	*outline_material;
};

struct GreasePencilShrinkwrapModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	Object	*target;
	Object	*aux_target;
	float	keep_dist;
	short	shrink_type;
	char	shrink_opts;
	char	shrink_mode;
	float	proj_limit;
	char	proj_axis;
	char	subsurf_levels;
	char	_pad[2];
	float	smooth_factor;
	int	smooth_step;
	void	*cache_data;
};

struct GreasePencilBuildModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	float	start_frame;
	float	end_frame;
	float	start_delay;
	float	length;
	short	flag;
	short	mode;
	short	transition;
	short	time_alignment;
	float	speed_fac;
	float	speed_maxgap;
	short	time_mode;
	char	_pad[6];
	Object	*object;
	float	percentage_fac;
	float	fade_fac;
	char	target_vgname[64];
	float	fade_opacity_strength;
	float	fade_thickness_strength;
};

struct GreasePencilSimplifyModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	short	mode;
	char	_pad[4];
	short	step;
	float	factor;
	float	length;
	float	sharp_threshold;
	float	distance;
};

struct GreasePencilTextureModifierData	{
	ModifierData	modifier;
	GreasePencilModifierInfluenceData	influence;
	float	uv_offset;
	float	uv_scale;
	float	fill_rotation;
	float	fill_offset[2];
	float	fill_scale;
	int	layer_pass;
	short	fit_method;
	short	mode;
	float	alignment_rotation;
	char	_pad[4];
};

struct MovieClipProxy	{
	char	dir[768];
	short	tc;
	short	quality;
	short	build_size_flag;
	short	build_tc_flag;
};

struct MovieClip_RuntimeGPUTexture	{
	void	*next;
	void	*prev;
	MovieClipUser	user;
	void	*gputexture[3];
};

struct MovieClip_Runtime	{
	ListBase	gputextures;
	uint64_t	last_update;
};

struct MovieTrackingSettings	{
	short	default_motion_model;
	short	default_algorithm_flag;
	float	default_minimum_correlation;
	short	default_pattern_size;
	short	default_search_size;
	short	default_frames_limit;
	short	default_margin;
	short	default_pattern_match;
	short	default_flag;
	float	default_weight;
	short	motion_flag;
	short	speed;
	int	keyframe1;
	int	keyframe2;
	int	reconstruction_flag;
	int	refine_camera_intrinsics;
	float	dist;
	int	clean_frames;
	int	clean_action;
	float	clean_error;
	float	object_distance;
};

struct MovieTrackingCamera	{
	void	*intrinsics;
	short	distortion_model;
	char	_pad[2];
	float	sensor_width;
	float	pixel_aspect;
	float	focal;
	short	units;
	char	_pad1[2];
	float	principal_point[2];
	float	principal[2];
	float	k1;
	float	k2;
	float	k3;
	float	division_k1;
	float	division_k2;
	float	nuke_k1;
	float	nuke_k2;
	float	brown_k1;
	float	brown_k2;
	float	brown_k3;
	float	brown_k4;
	float	brown_p1;
	float	brown_p2;
};

struct MovieReconstructedCamera;
struct MovieTrackingReconstruction	{
	int	flag;
	float	error;
	int	last_camera;
	int	camnr;
	MovieReconstructedCamera	*cameras;
};

struct MovieTrackingTrack;
struct MovieTrackingStabilization	{
	int	flag;
	int	tot_track;
	int	act_track;
	int	tot_rot_track;
	int	act_rot_track;
	float	maxscale;
	MovieTrackingTrack	*rot_track;
	int	anchor_frame;
	float	target_pos[2];
	float	target_rot;
	float	scale;
	float	locinf;
	float	scaleinf;
	float	rotinf;
	int	filter;
	int	_pad;
};

struct MovieTrackingPlaneTrack;
struct MovieTrackingStats;
struct MovieTrackingDopesheet	{
	int	ok;
	short	sort_method;
	short	flag;
	ListBase	coverage_segments;
	ListBase	channels;
	int	tot_channel;
	char	_pad[4];
};

struct MovieTracking	{
	MovieTrackingSettings	settings;
	MovieTrackingCamera	camera;
	ListBase	tracks;
	ListBase	plane_tracks;
	MovieTrackingReconstruction	reconstruction;
	MovieTrackingStabilization	stabilization;
	MovieTrackingTrack	*act_track;
	MovieTrackingPlaneTrack	*act_plane_track;
	ListBase	objects;
	int	objectnr;
	int	tot_object;
	MovieTrackingStats	*stats;
	MovieTrackingDopesheet	dopesheet;
};

struct MovieClip	{
	ID	id;
	AnimData	*adt;
	char	name[1024];
	int	source;
	int	_pad;
	int	lastsize[2];
	float	aspx;
	float	aspy;
	void	*anim;
	void	*cache;
	bGPdata	*gpd;
	void	*_pad1;
	MovieTracking	tracking;
	void	*tracking_context;
	MovieClipProxy	proxy;
	int	flag;
	int	len;
	int	start_frame;
	int	frame_offset;
	ColorManagedColorspaceSettings	colorspace_settings;
	MovieClip_Runtime	runtime;
};

struct MovieTrackingMarker	{
	float	pos[2];
	float	pattern_corners[4][2];
	float	search_min[2];
	float	search_max[2];
	int	framenr;
	int	flag;
};

struct MovieClipScopes	{
	short	ok;
	short	use_track_mask;
	int	track_preview_height;
	int	frame_width;
	int	frame_height;
	MovieTrackingMarker	undist_marker;
	void	*track_search;
	void	*track_preview;
	float	track_pos[2];
	short	track_disabled;
	short	track_locked;
	int	scene_framenr;
	MovieTrackingTrack	*track;
	MovieTrackingMarker	*marker;
	float	slide_scale[2];
};

struct bActionModifier	{
	bActionModifier	*next;
	bActionModifier	*prev;
	short	type;
	short	flag;
	char	channel[32];
	float	noisesize;
	float	turbul;
	short	channels;
	short	no_rot_axis;
	Object	*ob;
};

struct bActionStrip	{
	bActionStrip	*next;
	bActionStrip	*prev;
	short	flag;
	short	mode;
	short	stride_axis;
	short	curmod;
	Ipo	*ipo;
	bAction	*act;
	Object	*object;
	float	start;
	float	end;
	float	actstart;
	float	actend;
	float	actoffs;
	float	stridelen;
	float	repeat;
	float	scale;
	float	blendin;
	float	blendout;
	char	stridechannel[32];
	char	offs_bone[32];
	ListBase	modifiers;
};

struct bNodeTreeInterfaceItem	{
	char	item_type;
	char	_pad[7];
};

struct bNodeTreeInterfaceSocket	{
	bNodeTreeInterfaceItem	item;
	char	*name;
	char	*description;
	char	*socket_type;
	int	flag;
	short	attribute_domain;
	short	default_input;
	char	*default_attribute_name;
	char	*identifier;
	void	*socket_data;
	IDProperty	*properties;
	int8_t	structure_type;
	char	_pad[7];
};

struct bNodeTreeInterfacePanel	{
	bNodeTreeInterfaceItem	item;
	char	*name;
	char	*description;
	int	flag;
	char	_pad[4];
	bNodeTreeInterfaceItem	**items_array;
	int	items_num;
	int	identifier;
};

struct bNodeTreeInterface	{
	bNodeTreeInterfacePanel	root_panel;
	int	active_index;
	int	next_uid;
	void	*runtime;
};

struct bNodeStack	{
	float	vec[4];
	float	min;
	float	max;
	void	*data;
	short	hasinput;
	short	hasoutput;
	short	datatype;
	short	sockettype;
	short	is_copy;
	short	external;
	char	_pad[4];
};

struct bNodeLink;
struct bNodeSocket	{
	bNodeSocket	*next;
	bNodeSocket	*prev;
	IDProperty	*prop;
	char	identifier[64];
	char	name[64];
	void	*storage;
	short	type;
	short	flag;
	short	limit;
	short	in_out;
	void	*typeinfo;
	char	idname[64];
	void	*default_value;
	short	stack_index;
	char	display_shape;
	char	attribute_domain;
	char	_pad[4];
	char	label[64];
	char	short_label[64];
	char	description[64];
	char	*default_attribute_name;
	int	own_index;
	int	to_index;
	bNodeLink	*link;
	bNodeStack	ns;
	void	*runtime;
};

struct bNodePanelState	{
	int	identifier;
	char	flag;
	char	_pad[3];
};

struct bNode	{
	bNode	*next;
	bNode	*prev;
	ListBase	inputs;
	ListBase	outputs;
	char	name[64];
	int	identifier;
	int	flag;
	char	idname[64];
	void	*typeinfo;
	short	type;
	short	ui_order;
	short	custom1;
	short	custom2;
	float	custom3;
	float	custom4;
	int8_t	warning_propagation;
	char	_pad[7];
	ID	*id;
	void	*storage;
	IDProperty	*prop;
	bNode	*parent;
	float	location[2];
	float	width;
	float	height;
	float	locx;
	float	locy;
	float	offsetx;
	float	offsety;
	char	label[64];
	float	color[3];
	int	num_panel_states;
	bNodePanelState	*panel_states_array;
	void	*runtime;
};

struct bNodeInstanceKey	{
	int	value;
};

struct bNodeLink	{
	bNodeLink	*next;
	bNodeLink	*prev;
	bNode	*fromnode;
	bNode	*tonode;
	bNodeSocket	*fromsock;
	bNodeSocket	*tosock;
	int	flag;
	int	multi_input_socket_index;
};

struct bNestedNodePath	{
	int	node_id;
	int	id_in_node;
};

struct bNestedNodeRef	{
	int	id;
	char	_pad[4];
	bNestedNodePath	path;
};

struct GeometryNodeAssetTraits;
struct bNodeTree	{
	ID	id;
	AnimData	*adt;
	ID	*owner_id;
	void	*typeinfo;
	char	idname[64];
	char	*description;
	bGPdata	*gpd;
	float	view_center[2];
	ListBase	nodes;
	ListBase	links;
	int	type;
	int	cur_index;
	int	flag;
	int	chunksize;
	int	execution_mode;
	int	precision;
	int	color_tag;
	int	default_group_node_width;
	rctf	viewer_border;
	ListBase	inputs;
	ListBase	outputs;
	bNodeTreeInterface	tree_interface;
	bNodeInstanceKey	active_viewer_key;
	int	nested_node_refs_num;
	bNestedNodeRef	*nested_node_refs;
	GeometryNodeAssetTraits	*geometry_node_asset_traits;
	PreviewImage	*preview;
	void	*runtime;
};

struct bNodeSocketValueInt	{
	int	subtype;
	int	value;
	int	min;
	int	max;
};

struct bNodeSocketValueFloat	{
	int	subtype;
	float	value;
	float	min;
	float	max;
};

struct bNodeSocketValueBoolean	{
	char	value;
};

struct bNodeSocketValueVector	{
	int	subtype;
	float	value[4];
	float	min;
	float	max;
	int	dimensions;
};

struct bNodeSocketValueRotation	{
	float	value_euler[3];
};

struct bNodeSocketValueRGBA	{
	float	value[4];
};

struct bNodeSocketValueString	{
	int	subtype;
	char	_pad[4];
	char	value[1024];
};

struct bNodeSocketValueObject	{
	Object	*value;
};

struct bNodeSocketValueImage	{
	Image	*value;
};

struct bNodeSocketValueCollection	{
	Collection	*value;
};

struct bNodeSocketValueTexture	{
	Tex	*value;
};

struct bNodeSocketValueMaterial	{
	Material	*value;
};

struct bNodeSocketValueMenu	{
	int	value;
	int	runtime_flag;
	void	*enum_items;
};

struct GeometryNodeAssetTraits	{
	int	flag;
};

struct NodeFrame	{
	short	flag;
	short	label_size;
};

struct NodeReroute	{
	char	type_idname[64];
};

struct NodeImageAnim	{
	int	frames;
	int	sfra;
	int	nr;
	char	cyclic;
	char	movie;
	char	_pad[2];
};

struct ColorCorrectionData	{
	float	saturation;
	float	contrast;
	float	gamma;
	float	gain;
	float	lift;
	char	_pad[4];
};

struct NodeColorCorrection	{
	ColorCorrectionData	master;
	ColorCorrectionData	shadows;
	ColorCorrectionData	midtones;
	ColorCorrectionData	highlights;
	float	startmidtones;
	float	endmidtones;
};

struct NodeBokehImage	{
	float	angle;
	int	flaps;
	float	rounding;
	float	catadioptric;
	float	lensshift;
};

struct NodeBoxMask	{
	float	x;
	float	y;
	float	rotation;
	float	height;
	float	width;
	char	_pad[4];
};

struct NodeEllipseMask	{
	float	x;
	float	y;
	float	rotation;
	float	height;
	float	width;
	char	_pad[4];
};

struct NodeImageLayer	{
	int	pass_index;
	char	pass_name[64];
};

struct NodeBlurData	{
	short	sizex;
	short	sizey;
	short	samples;
	short	maxspeed;
	short	minspeed;
	short	relative;
	short	aspect;
	short	curved;
	float	fac;
	float	percentx;
	float	percenty;
	short	filtertype;
	char	bokeh;
	char	gamma;
	int	image_in_width;
	int	image_in_height;
};

struct NodeDBlurData	{
	float	center_x;
	float	center_y;
	float	distance;
	float	angle;
	float	spin;
	float	zoom;
	short	iter;
	char	_pad[2];
};

struct NodeBilateralBlurData	{
	float	sigma_color;
	float	sigma_space;
	short	iter;
	char	_pad[2];
};

struct NodeKuwaharaData	{
	short	size;
	short	variation;
	int	uniformity;
	float	sharpness;
	float	eccentricity;
	char	high_precision;
	char	_pad[3];
};

struct NodeAntiAliasingData	{
	float	threshold;
	float	contrast_limit;
	float	corner_rounding;
};

struct NodeHueSat	{
	float	hue;
	float	sat;
	float	val;
};

struct Stereo3dFormat	{
	short	flag;
	char	display_mode;
	char	anaglyph_type;
	char	interlace_type;
	char	_pad[3];
};

struct ImageFormatData	{
	char	imtype;
	char	depth;
	char	planes;
	char	flag;
	char	quality;
	char	compress;
	char	exr_codec;
	char	cineon_flag;
	short	cineon_white;
	short	cineon_black;
	float	cineon_gamma;
	char	jp2_flag;
	char	jp2_codec;
	char	tiff_codec;
	char	_pad[4];
	char	views_format;
	Stereo3dFormat	stereo3d_format;
	char	color_management;
	char	_pad1[7];
	ColorManagedViewSettings	view_settings;
	ColorManagedDisplaySettings	display_settings;
	ColorManagedColorspaceSettings	linear_colorspace_settings;
};

struct NodeImageFile	{
	char	name[1024];
	ImageFormatData	im_format;
	int	sfra;
	int	efra;
};

struct NodeImageMultiFile	{
	char	base_path[1024];
	ImageFormatData	format;
	int	sfra;
	int	efra;
	int	active_input;
	char	save_as_render;
	char	_pad[3];
};

struct NodeImageMultiFileSocket	{
	short	use_render_format;
	short	use_node_format;
	char	save_as_render;
	char	_pad1[3];
	char	path[1024];
	ImageFormatData	format;
	char	layer[62];
	char	_pad2[2];
};

struct NodeChroma	{
	float	t1;
	float	t2;
	float	t3;
	float	fsize;
	float	fstrength;
	float	falpha;
	float	key[4];
	short	algorithm;
	short	channel;
};

struct NodeTwoXYs	{
	short	x1;
	short	x2;
	short	y1;
	short	y2;
	float	fac_x1;
	float	fac_x2;
	float	fac_y1;
	float	fac_y2;
};

struct NodeTwoFloats	{
	float	x;
	float	y;
};

struct NodeVertexCol	{
	char	name[64];
};

struct NodeCMPCombSepColor	{
	uchar	mode;
	uchar	ycc_mode;
};

struct NodeDefocus	{
	char	bktype;
	char	_pad0;
	char	preview;
	char	gamco;
	short	samples;
	short	no_zbuf;
	float	fstop;
	float	maxblur;
	float	bthresh;
	float	scale;
	float	rotation;
	char	_pad1[4];
};

struct NodeScriptDict	{
	void	*dict;
	void	*node;
};

struct NodeGlare	{
	char	type;
	char	quality;
	char	iter;
	char	angle;
	char	_pad0;
	char	size;
	char	star_45;
	char	streaks;
	float	colmod;
	float	mix;
	float	threshold;
	float	fade;
	float	angle_ofs;
	char	_pad1[4];
};

struct NodeTonemap	{
	float	key;
	float	offset;
	float	gamma;
	float	f;
	float	m;
	float	a;
	float	c;
	int	type;
};

struct NodeLensDist	{
	short	jit;
	short	proj;
	short	fit;
	char	_pad[2];
	int	distortion_type;
};

struct NodeColorBalance	{
	float	slope[3];
	float	offset[3];
	float	power[3];
	float	offset_basis;
	char	_pad[4];
	float	lift[3];
	float	gamma[3];
	float	gain[3];
	float	input_temperature;
	float	input_tint;
	float	output_temperature;
	float	output_tint;
};

struct NodeColorspill	{
	short	limchan;
	short	unspill;
	float	limscale;
	float	uspillr;
	float	uspillg;
	float	uspillb;
};

struct NodeConvertColorSpace	{
	char	from_color_space[64];
	char	to_color_space[64];
};

struct NodeDilateErode	{
	char	falloff;
};

struct NodeMask	{
	int	size_x;
	int	size_y;
};

struct NodeSetAlpha	{
	char	mode;
};

struct TexMapping	{
	float	loc[3];
	float	rot[3];
	float	size[3];
	int	flag;
	char	projx;
	char	projy;
	char	projz;
	char	mapping;
	int	type;
	float	mat[4][4];
	float	min[3];
	float	max[3];
	Object	*ob;
};

struct ColorMapping	{
	ColorBand	coba;
	float	bright;
	float	contrast;
	float	saturation;
	int	flag;
	float	blend_color[3];
	float	blend_factor;
	int	blend_type;
	char	_pad[4];
};

struct NodeTexBase	{
	TexMapping	tex_mapping;
	ColorMapping	color_mapping;
};

struct NodeTexSky	{
	NodeTexBase	base;
	int	sky_model;
	float	sun_direction[3];
	float	turbidity;
	float	ground_albedo;
	float	sun_size;
	float	sun_intensity;
	float	sun_elevation;
	float	sun_rotation;
	float	altitude;
	float	air_density;
	float	dust_density;
	float	ozone_density;
	char	sun_disc;
	char	_pad[7];
};

struct NodeTexImage	{
	NodeTexBase	base;
	ImageUser	iuser;
	int	color_space;
	int	projection;
	float	projection_blend;
	int	interpolation;
	int	extension;
	char	_pad[4];
};

struct NodeTexChecker	{
	NodeTexBase	base;
};

struct NodeTexBrick	{
	NodeTexBase	base;
	int	offset_freq;
	int	squash_freq;
	float	offset;
	float	squash;
};

struct NodeTexEnvironment	{
	NodeTexBase	base;
	ImageUser	iuser;
	int	color_space;
	int	projection;
	int	interpolation;
	char	_pad[4];
};

struct NodeTexGabor	{
	NodeTexBase	base;
	char	type;
	char	_pad[7];
};

struct NodeTexGradient	{
	NodeTexBase	base;
	int	gradient_type;
	char	_pad[4];
};

struct NodeTexNoise	{
	NodeTexBase	base;
	int	dimensions;
	uchar	type;
	uchar	normalize;
	char	_pad[2];
};

struct NodeTexVoronoi	{
	NodeTexBase	base;
	int	dimensions;
	int	feature;
	int	distance;
	int	normalize;
	int	coloring;
	char	_pad[4];
};

struct NodeTexMusgrave	{
	NodeTexBase	base;
	int	musgrave_type;
	int	dimensions;
};

struct NodeTexWave	{
	NodeTexBase	base;
	int	wave_type;
	int	bands_direction;
	int	rings_direction;
	int	wave_profile;
};

struct NodeTexMagic	{
	NodeTexBase	base;
	int	depth;
	char	_pad[4];
};

struct NodeShaderAttribute	{
	char	name[256];
	int	type;
	char	_pad[4];
};

struct NodeShaderVectTransform	{
	int	type;
	int	convert_from;
	int	convert_to;
	char	_pad[4];
};

struct PointDensity	{
	short	flag;
	short	falloff_type;
	float	falloff_softness;
	float	radius;
	short	source;
	char	_pad0[2];
	short	color_source;
	short	ob_color_source;
	int	totpoints;
	Object	*object;
	int	psys;
	short	psys_cache_space;
	short	ob_cache_space;
	char	vertex_attribute_name[68];
	char	_pad1[4];
	void	*point_tree;
	float	*point_data;
	float	noise_size;
	short	noise_depth;
	short	noise_influence;
	short	noise_basis;
	char	_pad2[6];
	float	noise_fac;
	float	speed_scale;
	float	falloff_speed_scale;
	char	_pad3[4];
	ColorBand	*coba;
	CurveMapping	*falloff_curve;
};

struct NodeShaderTexPointDensity	{
	NodeTexBase	base;
	short	point_source;
	char	_pad[2];
	int	particle_system;
	float	radius;
	int	resolution;
	short	space;
	short	interpolation;
	short	color_source;
	short	ob_color_source;
	PointDensity	pd;
	int	cached_resolution;
	char	vertex_attribute_name[68];
};

struct NodeShaderPrincipled	{
	char	use_subsurface_auto_radius;
	char	_pad[3];
};

struct NodeShaderHairPrincipled	{
	short	model;
	short	parametrization;
	char	_pad[4];
};

struct TexNodeOutput	{
	char	name[64];
};

struct NodeKeyingScreenData	{
	char	tracking_object[64];
	float	smoothness;
};

struct NodeKeyingData	{
	float	screen_balance;
	float	despill_factor;
	float	despill_balance;
	int	edge_kernel_radius;
	float	edge_kernel_tolerance;
	float	clip_black;
	float	clip_white;
	int	dilate_distance;
	int	feather_distance;
	int	feather_falloff;
	int	blur_pre;
	int	blur_post;
};

struct NodeTrackPosData	{
	char	tracking_object[64];
	char	track_name[64];
};

struct NodeTranslateData	{
	char	wrap_axis;
	char	relative;
	short	interpolation;
};

struct NodeScaleData	{
	short	interpolation;
};

struct NodePlaneTrackDeformData	{
	char	tracking_object[64];
	char	plane_track_name[64];
	char	flag;
	char	motion_blur_samples;
	char	_pad[2];
	float	motion_blur_shutter;
};

struct NodeShaderScript	{
	int	mode;
	int	flag;
	char	filepath[1024];
	char	bytecode_hash[64];
	char	*bytecode;
};

struct NodeShaderTangent	{
	int	direction_type;
	int	axis;
	char	uv_map[64];
};

struct NodeShaderNormalMap	{
	int	space;
	char	uv_map[64];
};

struct NodeShaderUVMap	{
	char	uv_map[64];
};

struct NodeShaderVertexColor	{
	char	layer_name[64];
};

struct NodeShaderTexIES	{
	int	mode;
	char	filepath[1024];
};

struct NodeShaderOutputAOV	{
	char	name[64];
};

struct NodeSunBeams	{
	float	source[2];
	float	ray_length;
};

struct CryptomatteEntry	{
	CryptomatteEntry	*next;
	CryptomatteEntry	*prev;
	float	encoded_hash;
	char	name[64];
	char	_pad[4];
};

struct CryptomatteLayer	{
	CryptomatteEntry	*next;
	CryptomatteEntry	*prev;
	char	name[64];
};

struct NodeCryptomatte_Runtime	{
	ListBase	layers;
	float	add[3];
	float	remove[3];
};

struct NodeCryptomatte	{
	ImageUser	iuser;
	ListBase	entries;
	char	layer_name[64];
	char	*matte_id;
	int	num_inputs;
	char	_pad[4];
	NodeCryptomatte_Runtime	runtime;
};

struct NodeDenoise	{
	char	hdr;
	char	prefilter;
	char	quality;
	char	_pad[1];
};

struct NodeMapRange	{
	uchar	data_type;
	uchar	interpolation_type;
	uchar	clamp;
	char	_pad[5];
};

struct NodeRandomValue	{
	uchar	data_type;
};

struct NodeAccumulateField	{
	uchar	data_type;
	uchar	domain;
};

struct NodeInputBool	{
	uchar	boolean;
};

struct NodeInputInt	{
	int	integer;
};

struct NodeInputRotation	{
	float	rotation_euler[3];
};

struct NodeInputVector	{
	float	vector[3];
};

struct NodeInputColor	{
	float	color[4];
};

struct NodeInputString	{
	char	*string;
};

struct NodeGeometryExtrudeMesh	{
	uchar	mode;
};

struct NodeGeometryObjectInfo	{
	uchar	transform_space;
};

struct NodeGeometryPointsToVolume	{
	uchar	resolution_mode;
	uchar	input_type_radius;
};

struct NodeGeometryCollectionInfo	{
	uchar	transform_space;
};

struct NodeGeometryProximity	{
	uchar	target_element;
};

struct NodeGeometryVolumeToMesh	{
	uchar	resolution_mode;
};

struct NodeGeometryMeshToVolume	{
	uchar	resolution_mode;
};

struct NodeGeometrySubdivisionSurface	{
	uchar	uv_smooth;
	uchar	boundary_smooth;
};

struct NodeGeometryMeshCircle	{
	uchar	fill_type;
};

struct NodeGeometryMeshCylinder	{
	uchar	fill_type;
};

struct NodeGeometryMeshCone	{
	uchar	fill_type;
};

struct NodeGeometryMergeByDistance	{
	uchar	mode;
};

struct NodeGeometryMeshLine	{
	uchar	mode;
	uchar	count_mode;
};

struct NodeSwitch	{
	uchar	input_type;
};

struct NodeEnumItem	{
	char	*name;
	char	*description;
	int	identifier;
	char	_pad[4];
};

struct NodeEnumDefinition	{
	NodeEnumItem	*items_array;
	int	items_num;
	int	active_index;
	int	next_identifier;
	char	_pad[4];
};

struct NodeMenuSwitch	{
	NodeEnumDefinition	enum_definition;
	uchar	data_type;
	char	_pad[7];
};

struct NodeGeometryCurveSplineType	{
	uchar	spline_type;
};

struct NodeGeometrySetCurveHandlePositions	{
	uchar	mode;
};

struct NodeGeometryCurveSetHandles	{
	uchar	handle_type;
	uchar	mode;
};

struct NodeGeometryCurveSelectHandles	{
	uchar	handle_type;
	uchar	mode;
};

struct NodeGeometryCurvePrimitiveArc	{
	uchar	mode;
};

struct NodeGeometryCurvePrimitiveLine	{
	uchar	mode;
};

struct NodeGeometryCurvePrimitiveBezierSegment	{
	uchar	mode;
};

struct NodeGeometryCurvePrimitiveCircle	{
	uchar	mode;
};

struct NodeGeometryCurvePrimitiveQuad	{
	uchar	mode;
};

struct NodeGeometryCurveResample	{
	uchar	mode;
	uchar	keep_last_segment;
};

struct NodeGeometryCurveFillet	{
	uchar	mode;
};

struct NodeGeometryCurveTrim	{
	uchar	mode;
};

struct NodeGeometryCurveToPoints	{
	uchar	mode;
};

struct NodeGeometryCurveSample	{
	uchar	mode;
	int8_t	use_all_curves;
	int8_t	data_type;
	char	_pad[1];
};

struct NodeGeometryTransferAttribute	{
	int8_t	data_type;
	int8_t	domain;
	uchar	mode;
	char	_pad[1];
};

struct NodeGeometrySampleIndex	{
	int8_t	data_type;
	int8_t	domain;
	int8_t	clamp;
	char	_pad[1];
};

struct NodeGeometryRaycast	{
	uchar	mapping;
	int8_t	data_type;
};

struct NodeGeometryCurveFill	{
	uchar	mode;
};

struct NodeGeometryMeshToPoints	{
	uchar	mode;
};

struct NodeGeometryAttributeCaptureItem	{
	int8_t	data_type;
	char	_pad[3];
	int	identifier;
	char	*name;
};

struct NodeGeometryAttributeCapture	{
	int8_t	data_type;
	int8_t	domain;
	char	_pad[2];
	int	next_identifier;
	NodeGeometryAttributeCaptureItem	*capture_items;
	int	capture_items_num;
	int	active_index;
};

struct NodeGeometryStoreNamedAttribute	{
	int8_t	data_type;
	int8_t	domain;
};

struct NodeGeometryInputNamedAttribute	{
	int8_t	data_type;
};

struct NodeGeometryStringToCurves	{
	uchar	overflow;
	uchar	align_x;
	uchar	align_y;
	uchar	pivot_mode;
};

struct NodeGeometryDeleteGeometry	{
	int8_t	domain;
	int8_t	mode;
};

struct NodeGeometryDuplicateElements	{
	int8_t	domain;
};

struct NodeGeometryMergeLayers	{
	int8_t	mode;
};

struct NodeGeometrySeparateGeometry	{
	int8_t	domain;
};

struct NodeGeometryImageTexture	{
	int8_t	interpolation;
	int8_t	extension;
};

struct NodeGeometryViewer	{
	int8_t	data_type;
	int8_t	domain;
};

struct NodeGeometryUVUnwrap	{
	uchar	method;
};

struct NodeSimulationItem	{
	char	*name;
	short	socket_type;
	short	attribute_domain;
	int	identifier;
};

struct NodeGeometrySimulationInput	{
	int	output_node_id;
};

struct NodeGeometrySimulationOutput	{
	NodeSimulationItem	*items;
	int	items_num;
	int	active_index;
	int	next_identifier;
	int	_pad;
};

struct NodeRepeatItem	{
	char	*name;
	short	socket_type;
	char	_pad[2];
	int	identifier;
};

struct NodeGeometryRepeatInput	{
	int	output_node_id;
};

struct NodeGeometryRepeatOutput	{
	NodeRepeatItem	*items;
	int	items_num;
	int	active_index;
	int	next_identifier;
	int	inspection_index;
};

struct NodeGeometryForeachGeometryElementInput	{
	int	output_node_id;
};

struct NodeForeachGeometryElementInputItem	{
	char	*name;
	short	socket_type;
	char	_pad[2];
	int	identifier;
};

struct NodeForeachGeometryElementMainItem	{
	char	*name;
	short	socket_type;
	char	_pad[2];
	int	identifier;
};

struct NodeForeachGeometryElementGenerationItem	{
	char	*name;
	short	socket_type;
	uchar	domain;
	char	_pad[1];
	int	identifier;
};

struct NodeForeachGeometryElementInputItems	{
	NodeForeachGeometryElementInputItem	*items;
	int	items_num;
	int	active_index;
	int	next_identifier;
	char	_pad[4];
};

struct NodeForeachGeometryElementMainItems	{
	NodeForeachGeometryElementMainItem	*items;
	int	items_num;
	int	active_index;
	int	next_identifier;
	char	_pad[4];
};

struct NodeForeachGeometryElementGenerationItems	{
	NodeForeachGeometryElementGenerationItem	*items;
	int	items_num;
	int	active_index;
	int	next_identifier;
	char	_pad[4];
};

struct NodeGeometryForeachGeometryElementOutput	{
	NodeForeachGeometryElementInputItems	input_items;
	NodeForeachGeometryElementMainItems	main_items;
	NodeForeachGeometryElementGenerationItems	generation_items;
	int	inspection_index;
	uchar	domain;
	char	_pad[3];
};

struct NodeGeometryClosureInput	{
	int	output_node_id;
};

struct NodeGeometryClosureInputItem	{
	char	*name;
	short	socket_type;
	char	_pad[2];
	int	identifier;
};

struct NodeGeometryClosureOutputItem	{
	char	*name;
	short	socket_type;
	char	_pad[2];
	int	identifier;
};

struct NodeGeometryClosureInputItems	{
	NodeGeometryClosureInputItem	*items;
	int	items_num;
	int	active_index;
	int	next_identifier;
	char	_pad[4];
};

struct NodeGeometryClosureOutputItems	{
	NodeGeometryClosureOutputItem	*items;
	int	items_num;
	int	active_index;
	int	next_identifier;
	char	_pad[4];
};

struct NodeGeometryClosureOutput	{
	NodeGeometryClosureInputItems	input_items;
	NodeGeometryClosureOutputItems	output_items;
};

struct NodeGeometryEvaluateClosureInputItem	{
	char	*name;
	short	socket_type;
	char	_pad[2];
	int	identifier;
};

struct NodeGeometryEvaluateClosureOutputItem	{
	char	*name;
	short	socket_type;
	char	_pad[2];
	int	identifier;
};

struct NodeGeometryEvaluateClosureInputItems	{
	NodeGeometryEvaluateClosureInputItem	*items;
	int	items_num;
	int	active_index;
	int	next_identifier;
	char	_pad[4];
};

struct NodeGeometryEvaluateClosureOutputItems	{
	NodeGeometryEvaluateClosureOutputItem	*items;
	int	items_num;
	int	active_index;
	int	next_identifier;
	char	_pad[4];
};

struct NodeGeometryEvaluateClosure	{
	NodeGeometryEvaluateClosureInputItems	input_items;
	NodeGeometryEvaluateClosureOutputItems	output_items;
};

struct IndexSwitchItem	{
	int	identifier;
};

struct NodeIndexSwitch	{
	IndexSwitchItem	*items;
	int	items_num;
	int	data_type;
	int	next_identifier;
	char	_pad[4];
};

struct NodeGeometryDistributePointsInVolume	{
	uchar	mode;
};

struct NodeFunctionCompare	{
	int8_t	operation;
	int8_t	data_type;
	int8_t	mode;
	char	_pad[1];
};

struct NodeCombSepColor	{
	int8_t	mode;
};

struct NodeShaderMix	{
	int8_t	data_type;
	int8_t	factor_mode;
	int8_t	clamp_factor;
	int8_t	clamp_result;
	int8_t	blend_type;
	char	_pad[3];
};

struct NodeGeometryLinearGizmo	{
	int	color_id;
	int	draw_style;
};

struct NodeGeometryDialGizmo	{
	int	color_id;
};

struct NodeGeometryTransformGizmo	{
	int	flag;
};

struct NodeGeometryBakeItem	{
	char	*name;
	short	socket_type;
	short	attribute_domain;
	int	identifier;
	int	flag;
	char	_pad[4];
};

struct NodeGeometryBake	{
	NodeGeometryBakeItem	*items;
	int	items_num;
	int	next_identifier;
	int	active_index;
	char	_pad[4];
};

struct NodeGeometryCombineBundleItem	{
	char	*name;
	int	identifier;
	short	socket_type;
	char	_pad[2];
};

struct NodeGeometryCombineBundle	{
	NodeGeometryCombineBundleItem	*items;
	int	items_num;
	int	next_identifier;
	int	active_index;
	char	_pad[4];
};

struct NodeGeometrySeparateBundleItem	{
	char	*name;
	int	identifier;
	short	socket_type;
	char	_pad[2];
};

struct NodeGeometrySeparateBundle	{
	NodeGeometrySeparateBundleItem	*items;
	int	items_num;
	int	next_identifier;
	int	active_index;
	char	_pad[4];
};

struct NodeFunctionFormatStringItem	{
	char	*name;
	int	identifier;
	short	socket_type;
	char	_pad[2];
};

struct NodeFunctionFormatString	{
	NodeFunctionFormatStringItem	*items;
	int	items_num;
	int	next_identifier;
	int	active_index;
	char	_pad[4];
};

struct FluidVertexVelocity	{
	float	vel[3];
};

struct FluidsimSettings	{
	FluidsimModifierData	*fmd;
	int	threads;
	char	_pad1[4];
	short	type;
	short	show_advancedoptions;
	short	resolutionxyz;
	short	previewresxyz;
	float	realsize;
	short	guiDisplayMode;
	short	renderDisplayMode;
	float	viscosityValue;
	short	viscosityMode;
	short	viscosityExponent;
	float	grav[3];
	float	animStart;
	float	animEnd;
	int	bakeStart;
	int	bakeEnd;
	int	frameOffset;
	char	_pad2[4];
	float	gstar;
	int	maxRefine;
	float	iniVelx;
	float	iniVely;
	float	iniVelz;
	char	surfdataPath[1024];
	float	bbStart[3];
	float	bbSize[3];
	Ipo	*ipo;
	short	typeFlags;
	char	domainNovecgen;
	char	volumeInitType;
	float	partSlipValue;
	int	generateTracers;
	float	generateParticles;
	float	surfaceSmoothing;
	int	surfaceSubdivs;
	int	flag;
	float	particleInfSize;
	float	particleInfAlpha;
	float	farFieldSize;
	FluidVertexVelocity	*meshVelocities;
	int	totvert;
	float	cpsTimeStart;
	float	cpsTimeEnd;
	float	cpsQuality;
	float	attractforceStrength;
	float	attractforceRadius;
	float	velocityforceStrength;
	float	velocityforceRadius;
	int	lastgoodframe;
	float	animRate;
};

struct PartDeflect	{
	int	flag;
	short	deflect;
	short	forcefield;
	short	falloff;
	short	shape;
	short	tex_mode;
	short	kink;
	short	kink_axis;
	short	zdir;
	float	f_strength;
	float	f_damp;
	float	f_flow;
	float	f_wind_factor;
	char	_pad0[4];
	float	f_size;
	float	f_power;
	float	maxdist;
	float	mindist;
	float	f_power_r;
	float	maxrad;
	float	minrad;
	float	pdef_damp;
	float	pdef_rdamp;
	float	pdef_perm;
	float	pdef_frict;
	float	pdef_rfrict;
	float	pdef_stickness;
	float	absorption;
	float	pdef_sbdamp;
	float	pdef_sbift;
	float	pdef_sboft;
	float	clump_fac;
	float	clump_pow;
	float	kink_freq;
	float	kink_shape;
	float	kink_amp;
	float	free_end;
	float	tex_nabla;
	Tex	*tex;
	float	f_noise;
	int	seed;
	float	drawvec1[4];
	float	drawvec2[4];
	float	drawvec_falloff_min[3];
	char	_pad1[4];
	float	drawvec_falloff_max[3];
	char	_pad2[4];
	Object	*f_source;
	float	pdef_cfrict;
	char	_pad[4];
};

struct EffectorWeights	{
	Collection	*group;
	float	weight[14];
	float	global_gravity;
	short	flag;
	char	_pad[2];
};

struct SBVertex	{
	float	vec[4];
};

struct SoftBody_Shared	{
	PointCache	*pointcache;
	ListBase	ptcaches;
};

struct SoftBody	{
	int	totpoint;
	int	totspring;
	void	*bpoint;
	void	*bspring;
	char	_pad;
	char	msg_lock;
	short	msg_value;
	float	nodemass;
	char	namedVG_Mass[64];
	float	grav;
	float	mediafrict;
	float	rklimit;
	float	physics_speed;
	float	goalspring;
	float	goalfrict;
	float	mingoal;
	float	maxgoal;
	float	defgoal;
	short	vertgroup;
	char	namedVG_Softgoal[64];
	short	fuzzyness;
	float	inspring;
	float	infrict;
	char	namedVG_Spring_K[64];
	char	_pad1[6];
	char	local;
	char	solverflags;
	SBVertex	**keys;
	int	totpointkey;
	int	totkey;
	float	secondspring;
	float	colball;
	float	balldamp;
	float	ballstiff;
	short	sbc_mode;
	short	aeroedge;
	short	minloops;
	short	maxloops;
	short	choke;
	short	solver_ID;
	short	plastic;
	short	springpreload;
	void	*scratch;
	float	shearstiff;
	float	inpush;
	SoftBody_Shared	*shared;
	PointCache	*pointcache;
	ListBase	ptcaches;
	Collection	*collision_group;
	EffectorWeights	*effector_weights;
	float	lcom[3];
	float	lrot[3][3];
	float	lscale[3][3];
	int	last_frame;
};

struct bDeformGroup	{
	bDeformGroup	*next;
	bDeformGroup	*prev;
	char	name[64];
	char	flag;
	char	_pad0[7];
};

struct bFaceMap	{
	bFaceMap	*next;
	bFaceMap	*prev;
	char	name[64];
	char	flag;
	char	_pad0[7];
};

struct BoundBox	{
	float	vec[8][3];
};

struct ObjectLineArt	{
	short	usage;
	short	flags;
	float	crease_threshold;
	char	intersection_priority;
	char	_pad[7];
};

struct LightLinkingRuntime	{
	uint64_t	light_set_membership;
	uint64_t	shadow_set_membership;
	uchar	receiver_light_set;
	uchar	blocker_shadow_set;
	uchar	_pad[6];
};

struct LightLinking	{
	Collection	*receiver_collection;
	Collection	*blocker_collection;
	LightLinkingRuntime	runtime;
};

struct RigidBodyOb;
struct RigidBodyCon;
struct Object	{
	ID	id;
	AnimData	*adt;
	void	*sculpt;
	short	type;
	short	partype;
	int	par1;
	int	par2;
	int	par3;
	char	parsubstr[64];
	Object	*parent;
	Object	*track;
	Object	*proxy;
	Object	*proxy_group;
	Object	*proxy_from;
	Ipo	*ipo;
	bAction	*action;
	bAction	*poselib;
	bPose	*pose;
	void	*data;
	bGPdata	*gpd;
	bAnimVizSettings	avs;
	bMotionPath	*mpath;
	void	*_pad0;
	ListBase	constraintChannels;
	ListBase	effect;
	ListBase	defbase;
	ListBase	fmaps;
	ListBase	modifiers;
	ListBase	greasepencil_modifiers;
	ListBase	shader_fx;
	int	mode;
	int	restore_mode;
	Material	**mat;
	char	*matbits;
	int	totcol;
	int	actcol;
	float	loc[3];
	float	dloc[3];
	float	size[3];
	float	dsize[3];
	float	dscale[3];
	float	rot[3];
	float	drot[3];
	float	quat[4];
	float	dquat[4];
	float	rotAxis[3];
	float	drotAxis[3];
	float	rotAngle;
	float	drotAngle;
	float	parentinv[4][4];
	float	constinv[4][4];
	int	lay;
	short	flag;
	short	colbits;
	short	transflag;
	short	protectflag;
	short	trackflag;
	short	upflag;
	short	nlaflag;
	char	_pad1;
	char	duplicator_visibility_flag;
	short	base_flag;
	short	base_local_view_bits;
	short	col_group;
	short	col_mask;
	short	rotmode;
	char	boundtype;
	char	collision_boundtype;
	short	dtx;
	char	dt;
	char	empty_drawtype;
	float	empty_drawsize;
	float	dupfacesca;
	short	index;
	short	actdef;
	char	_pad2[4];
	float	col[4];
	short	softflag;
	short	restrictflag;
	short	shapenr;
	char	shapeflag;
	char	_pad3[1];
	ListBase	constraints;
	ListBase	nlastrips;
	ListBase	hooks;
	ListBase	particlesystem;
	PartDeflect	*pd;
	SoftBody	*soft;
	Collection	*dup_group;
	FluidsimSettings	*fluidsimSettings;
	ListBase	pc_ids;
	RigidBodyOb	*rigidbody_object;
	RigidBodyCon	*rigidbody_constraint;
	float	ima_ofs[2];
	ImageUser	*iuser;
	char	empty_image_visibility_flag;
	char	empty_image_depth;
	char	empty_image_flag;
	uchar	modifier_flag;
	float	shadow_terminator_normal_offset;
	float	shadow_terminator_geometry_offset;
	float	shadow_terminator_shading_offset;
	PreviewImage	*preview;
	ObjectLineArt	lineart;
	LightgroupMembership	*lightgroup;
	LightLinking	*light_linking;
	LightProbeObjectCache	*lightprobe_cache;
	void	*runtime;
};

struct ObHook	{
	ObHook	*next;
	ObHook	*prev;
	Object	*parent;
	float	parentinv[4][4];
	float	mat[4][4];
	float	cent[3];
	float	falloff;
	char	name[64];
	int	*indexar;
	int	totindex;
	int	curindex;
	short	type;
	short	active;
	float	force;
};

struct TreeStoreElem	{
	short	type;
	short	nr;
	short	flag;
	short	used;
	ID	*id;
};

struct TreeStore	{
	int	totelem;
	int	usedelem;
	TreeStoreElem	*data;
};

struct PackedFile	{
	int	size;
	int	seek;
	void	*data;
	void	*sharing_info;
};

struct HairKey	{
	float	co[3];
	float	time;
	float	weight;
	short	editflag;
	char	_pad[2];
	float	world_co[3];
};

struct ParticleKey	{
	float	co[3];
	float	vel[3];
	float	rot[4];
	float	ave[3];
	float	time;
};

struct BoidParticle	{
	Object	*ground;
	BoidData	data;
	float	gravity[3];
	float	wander[3];
	char	_pad0[4];
};

struct ParticleSpring	{
	float	rest_length;
	int	particle_index[2];
	int	delete_flag;
};

struct ChildParticle	{
	int	num;
	int	parent;
	int	pa[4];
	float	w[4];
	float	fuv[4];
	float	foffset;
	char	_pad0[4];
};

struct ParticleTarget	{
	ParticleTarget	*next;
	ParticleTarget	*prev;
	Object	*ob;
	int	psys;
	short	flag;
	short	mode;
	float	time;
	float	duration;
};

struct ParticleDupliWeight	{
	ParticleDupliWeight	*next;
	ParticleDupliWeight	*prev;
	Object	*ob;
	short	count;
	short	flag;
	short	index;
	char	_pad0[2];
};

struct ParticleData	{
	ParticleKey	state;
	ParticleKey	prev_state;
	HairKey	*hair;
	ParticleKey	*keys;
	BoidParticle	*boid;
	int	totkey;
	float	time;
	float	lifetime;
	float	dietime;
	int	num;
	int	num_dmcache;
	float	fuv[4];
	float	foffset;
	float	size;
	float	sphdensity;
	char	_pad[4];
	int	hair_index;
	short	flag;
	short	alive;
};

struct SPHFluidSettings	{
	float	radius;
	float	spring_k;
	float	rest_length;
	float	plasticity_constant;
	float	yield_ratio;
	float	plasticity_balance;
	float	yield_balance;
	float	viscosity_omega;
	float	viscosity_beta;
	float	stiffness_k;
	float	stiffness_knear;
	float	rest_density;
	float	buoyancy;
	int	flag;
	int	spring_frames;
	short	solver;
	char	_pad[6];
};

struct ParticleSettings	{
	ID	id;
	AnimData	*adt;
	BoidSettings	*boids;
	SPHFluidSettings	*fluid;
	EffectorWeights	*effector_weights;
	Collection	*collision_group;
	int	flag;
	char	_pad1[4];
	short	type;
	short	from;
	short	distr;
	short	texact;
	short	phystype;
	short	rotmode;
	short	avemode;
	short	reactevent;
	int	draw;
	float	draw_size;
	short	draw_as;
	short	childtype;
	char	_pad2[4];
	short	ren_as;
	short	subframes;
	short	draw_col;
	short	draw_step;
	short	ren_step;
	short	hair_step;
	short	keys_step;
	short	adapt_angle;
	short	adapt_pix;
	short	disp;
	short	omat;
	short	interpolation;
	short	integrator;
	short	rotfrom;
	short	kink;
	short	kink_axis;
	short	bb_align;
	short	bb_uv_split;
	short	bb_anim;
	short	bb_split_offset;
	float	bb_tilt;
	float	bb_rand_tilt;
	float	bb_offset[2];
	float	bb_size[2];
	float	bb_vel_head;
	float	bb_vel_tail;
	float	color_vec_max;
	float	sta;
	float	end;
	float	lifetime;
	float	randlife;
	float	timetweak;
	float	courant_target;
	float	jitfac;
	float	eff_hair;
	float	grid_rand;
	float	ps_offset[1];
	int	totpart;
	int	userjit;
	int	grid_res;
	int	effector_amount;
	short	time_flag;
	char	_pad0[6];
	float	normfac;
	float	obfac;
	float	randfac;
	float	partfac;
	float	tanfac;
	float	tanphase;
	float	reactfac;
	float	ob_vel[3];
	float	avefac;
	float	phasefac;
	float	randrotfac;
	float	randphasefac;
	float	mass;
	float	size;
	float	randsize;
	float	acc[3];
	float	dragfac;
	float	brownfac;
	float	dampfac;
	float	randlength;
	int	child_flag;
	char	_pad3[4];
	int	child_nbr;
	int	ren_child_nbr;
	float	parents;
	float	childsize;
	float	childrandsize;
	float	childrad;
	float	childflat;
	float	clumpfac;
	float	clumppow;
	float	kink_amp;
	float	kink_freq;
	float	kink_shape;
	float	kink_flat;
	float	kink_amp_clump;
	int	kink_extra_steps;
	char	_pad4[4];
	float	kink_axis_random;
	float	kink_amp_random;
	float	rough1;
	float	rough1_size;
	float	rough2;
	float	rough2_size;
	float	rough2_thres;
	float	rough_end;
	float	rough_end_shape;
	float	clength;
	float	clength_thres;
	float	parting_fac;
	float	parting_min;
	float	parting_max;
	float	branch_thres;
	float	draw_line[2];
	float	path_start;
	float	path_end;
	int	trail_count;
	int	keyed_loops;
	CurveMapping	*clumpcurve;
	CurveMapping	*roughcurve;
	float	clump_noise_size;
	float	bending_random;
	MTex	*mtex[18];
	Collection	*dup_group;
	ListBase	dupliweights;
	Collection	*force_group;
	Object	*dup_ob;
	Object	*bb_ob;
	Ipo	*ipo;
	PartDeflect	*pd;
	PartDeflect	*pd2;
	short	use_modifier_stack;
	char	_pad5[2];
	short	shape_flag;
	char	_pad6[2];
	float	twist;
	char	_pad8[4];
	float	shape;
	float	rad_root;
	float	rad_tip;
	float	rad_scale;
	CurveMapping	*twistcurve;
	void	*_pad7;
};

struct ParticleSystem	{
	ParticleSystem	*next;
	ParticleSystem	*prev;
	ParticleSettings	*part;
	ParticleData	*particles;
	ChildParticle	*child;
	void	*edit;
	void	(*free_edit)();
	void	**pathcache;
	void	**childcache;
	ListBase	pathcachebufs;
	ListBase	childcachebufs;
	ClothModifierData	*clmd;
	Mesh	*hair_in_mesh;
	Mesh	*hair_out_mesh;
	Object	*target_ob;
	void	*lattice_deform_data;
	Object	*parent;
	ListBase	targets;
	char	name[64];
	float	imat[4][4];
	float	cfra;
	float	tree_frame;
	float	bvhtree_frame;
	int	seed;
	int	child_seed;
	int	flag;
	int	totpart;
	int	totunexist;
	int	totchild;
	int	totcached;
	int	totchildcache;
	int	recalc;
	short	target_psys;
	short	totkeyed;
	short	bakespace;
	char	_pad1[6];
	char	bb_uvname[3][68];
	char	_pad2[4];
	short	vgroup[13];
	short	vg_neg;
	short	rt3;
	char	_pad3[6];
	PointCache	*pointcache;
	ListBase	ptcaches;
	ListBase	*effectors;
	ParticleSpring	*fluid_springs;
	int	tot_fluidsprings;
	int	alloc_fluidsprings;
	void	*tree;
	void	*bvhtree;
	void	*pdd;
	float	dt_frac;
	float	lattice_strength;
	void	*batch_cache;
	ParticleSystem	*orig_psys;
};

struct PTCacheExtra	{
	PTCacheExtra	*next;
	PTCacheExtra	*prev;
	int	type;
	int	totdata;
	void	*data;
};

struct PTCacheMem	{
	PTCacheMem	*next;
	PTCacheMem	*prev;
	int	frame;
	int	totpoint;
	int	data_types;
	int	flag;
	void	*data[8];
	ListBase	extradata;
};

struct PointCache	{
	PointCache	*next;
	PointCache	*prev;
	int	flag;
	int	step;
	int	simframe;
	int	startframe;
	int	endframe;
	int	editframe;
	int	last_exact;
	int	last_valid;
	char	_pad[4];
	int	totpoint;
	int	index;
	short	compression;
	char	_pad0[2];
	char	name[64];
	char	prev_name[64];
	char	info[128];
	char	path[1024];
	char	*cached_frames;
	int	cached_frames_len;
	char	_pad1[4];
	ListBase	mem_cache;
	void	*edit;
	void	(*free_edit)();
};

struct PointCloud	{
	ID	id;
	AnimData	*adt;
	int	flag;
	int	totpoint;
	AttributeStorage	attribute_storage;
	CustomData	pdata;
	int	attributes_active_index;
	int	_pad4;
	Material	**mat;
	short	totcol;
	short	_pad3[3];
	void	*runtime;
	void	*batch_cache;
};

struct RigidBodyWorld_Shared	{
	PointCache	*pointcache;
	ListBase	ptcaches;
	void	*runtime;
};

struct RigidBodyWorld	{
	EffectorWeights	*effector_weights;
	Collection	*group;
	Object	**objects;
	Collection	*constraints;
	char	_pad[4];
	float	ltime;
	RigidBodyWorld_Shared	*shared;
	PointCache	*pointcache;
	ListBase	ptcaches;
	int	numbodies;
	short	steps_per_second;
	short	num_solver_iterations;
	int	flag;
	float	time_scale;
};

struct RigidBodyOb	{
	short	type;
	short	shape;
	int	flag;
	int	col_groups;
	short	mesh_source;
	char	_pad[2];
	float	mass;
	float	friction;
	float	restitution;
	float	margin;
	float	lin_damping;
	float	ang_damping;
	float	lin_sleep_thresh;
	float	ang_sleep_thresh;
	float	orn[4];
	float	pos[3];
	char	_pad1[4];
	void	*shared;
};

struct RigidBodyCon	{
	Object	*ob1;
	Object	*ob2;
	short	type;
	short	num_solver_iterations;
	int	flag;
	float	breaking_threshold;
	char	spring_type;
	char	_pad[3];
	float	limit_lin_x_lower;
	float	limit_lin_x_upper;
	float	limit_lin_y_lower;
	float	limit_lin_y_upper;
	float	limit_lin_z_lower;
	float	limit_lin_z_upper;
	float	limit_ang_x_lower;
	float	limit_ang_x_upper;
	float	limit_ang_y_lower;
	float	limit_ang_y_upper;
	float	limit_ang_z_lower;
	float	limit_ang_z_upper;
	float	spring_stiffness_x;
	float	spring_stiffness_y;
	float	spring_stiffness_z;
	float	spring_stiffness_ang_x;
	float	spring_stiffness_ang_y;
	float	spring_stiffness_ang_z;
	float	spring_damping_x;
	float	spring_damping_y;
	float	spring_damping_z;
	float	spring_damping_ang_x;
	float	spring_damping_ang_y;
	float	spring_damping_ang_z;
	float	motor_lin_target_velocity;
	float	motor_ang_target_velocity;
	float	motor_lin_max_impulse;
	float	motor_ang_max_impulse;
	void	*physics_constraint;
};

struct FFMpegCodecData	{
	int	type;
	int	codec;
	int	audio_codec;
	int	video_bitrate;
	int	audio_bitrate;
	int	audio_mixrate;
	int	audio_channels;
	float	audio_volume;
	int	gop_size;
	int	max_b_frames;
	int	flags;
	int	constant_rate_factor;
	int	ffmpeg_preset;
	int	ffmpeg_prores_profile;
	int	rc_min_rate;
	int	rc_max_rate;
	int	rc_buffer_size;
	int	mux_packet_size;
	int	mux_rate;
	char	_pad0[4];
};

struct AudioData	{
	int	mixrate;
	float	main;
	float	speed_of_sound;
	float	doppler_factor;
	int	distance_model;
	short	flag;
	char	_pad[2];
	float	volume;
	char	_pad2[4];
};

struct SceneRenderLayer	{
	SceneRenderLayer	*next;
	SceneRenderLayer	*prev;
	char	name[64];
	Material	*mat_override;
	World	*world_override;
	int	lay;
	int	lay_zmask;
	int	lay_exclude;
	int	layflag;
	int	passflag;
	int	pass_xor;
	int	samples;
	float	pass_alpha_threshold;
	IDProperty	*prop;
	FreestyleConfig	freestyleConfig;
};

struct SceneRenderView	{
	SceneRenderView	*next;
	SceneRenderView	*prev;
	char	name[64];
	char	suffix[64];
	int	viewflag;
	char	_pad2[4];
};

struct BakeData	{
	ImageFormatData	im_format;
	char	filepath[1024];
	short	width;
	short	height;
	short	margin;
	short	flag;
	float	cage_extrusion;
	float	max_ray_distance;
	int	pass_filter;
	char	normal_swizzle[3];
	char	normal_space;
	char	target;
	char	save_mode;
	char	margin_type;
	char	view_from;
	char	_pad[4];
	Object	*cage_object;
};

struct RenderData	{
	ImageFormatData	im_format;
	FFMpegCodecData	ffcodecdata;
	int	cfra;
	int	sfra;
	int	efra;
	float	subframe;
	int	psfra;
	int	pefra;
	int	images;
	int	framapto;
	short	flag;
	short	threads;
	float	framelen;
	int	frame_step;
	short	dimensionspreset;
	short	size;
	int	xsch;
	int	ysch;
	int	tilex;
	int	tiley;
	short	planes;
	short	imtype;
	short	subimtype;
	short	quality;
	char	use_lock_interface;
	char	_pad7[3];
	int	scemode;
	int	mode;
	short	frs_sec;
	char	alphamode;
	char	_pad0[1];
	rctf	border;
	ListBase	layers;
	short	actlay;
	char	_pad1[2];
	float	xasp;
	float	yasp;
	float	ppm_factor;
	float	ppm_base;
	float	frs_sec_base;
	float	gauss;
	int	color_mgt_flag;
	float	dither_intensity;
	short	bake_mode;
	short	bake_flag;
	short	bake_filter;
	short	bake_samples;
	short	bake_margin_type;
	char	_pad9[6];
	float	bake_biasdist;
	float	bake_user_scale;
	char	pic[1024];
	int	stamp;
	short	stamp_font_id;
	char	_pad3[2];
	char	stamp_udata[768];
	float	fg_stamp[4];
	float	bg_stamp[4];
	char	seq_prev_type;
	char	seq_rend_type;
	char	seq_flag;
	char	_pad5[3];
	short	simplify_subsurf;
	short	simplify_subsurf_render;
	short	simplify_gpencil;
	float	simplify_particles;
	float	simplify_particles_render;
	float	simplify_volumes;
	int	line_thickness_mode;
	float	unit_line_thickness;
	char	engine[32];
	char	_pad2[2];
	short	perf_flag;
	BakeData	bake;
	int	_pad8;
	short	preview_pixel_size;
	short	_pad4;
	ListBase	views;
	short	actview;
	short	views_format;
	short	hair_type;
	short	hair_subdiv;
	float	blurfac;
	int	motion_blur_position;
	CurveMapping	mblur_shutter_curve;
	int	compositor_device;
	int	compositor_precision;
	int	compositor_denoise_device;
	int	compositor_denoise_preview_quality;
	int	compositor_denoise_final_quality;
	char	_pad6[4];
};

struct TimeMarker	{
	TimeMarker	*next;
	TimeMarker	*prev;
	int	frame;
	char	name[64];
	int	flag;
	Object	*camera;
	IDProperty	*prop;
};

struct Paint_Runtime	{
	int	initialized;
	short	ob_mode;
	char	_pad[2];
	AssetWeakReference	*previous_active_brush_reference;
};

struct NamedBrushAssetReference	{
	NamedBrushAssetReference	*next;
	NamedBrushAssetReference	*prev;
	char	*name;
	AssetWeakReference	*brush_asset_reference;
};

struct ToolSystemBrushBindings	{
	AssetWeakReference	*main_brush_asset_reference;
	ListBase	active_brush_per_brush_type;
};

struct Paint	{
	Brush	*brush;
	AssetWeakReference	*brush_asset_reference;
	Brush	*eraser_brush;
	AssetWeakReference	*eraser_brush_asset_reference;
	ToolSystemBrushBindings	tool_brush_bindings;
	Palette	*palette;
	CurveMapping	*cavity_curve;
	void	*paint_cursor;
	char	paint_cursor_col[4];
	int	flags;
	int	num_input_samples;
	int	symmetry_flags;
	float	tile_offset[3];
	char	_pad2[4];
	Paint_Runtime	runtime;
};

struct ImagePaintSettings	{
	Paint	paint;
	short	flag;
	short	missing_data;
	short	seam_bleed;
	short	normal_angle;
	short	screen_grab_size[2];
	int	mode;
	Image	*stencil;
	Image	*clone;
	Image	*canvas;
	float	stencil_col[3];
	float	dither;
	int	interp;
	char	_pad[4];
	float	clone_offset[2];
	float	clone_alpha;
	char	_pad2[4];
};

struct PaintModeSettings	{
	char	canvas_source;
	char	_pad[7];
	Image	*canvas_image;
	ImageUser	image_user;
};

struct ParticleBrushData	{
	short	size;
	short	step;
	short	invert;
	short	count;
	int	flag;
	float	strength;
};

struct ParticleEditSettings	{
	short	flag;
	short	totrekey;
	short	totaddkey;
	short	brushtype;
	ParticleBrushData	brush[7];
	void	*paintcursor;
	float	emitterdist;
	char	_pad0[4];
	int	selectmode;
	int	edittype;
	int	draw_step;
	int	fade_frames;
	Scene	*scene;
	Object	*object;
	Object	*shape_object;
};

struct Sculpt	{
	Paint	paint;
	int	flags;
	int	transform_mode;
	int	automasking_flags;
	int	radial_symm[3];
	float	detail_size;
	int	symmetrize_direction;
	float	gravity_factor;
	float	constant_detail;
	float	detail_percent;
	int	automasking_boundary_edges_propagation_steps;
	int	automasking_cavity_blur_steps;
	float	automasking_cavity_factor;
	float	automasking_start_normal_limit;
	float	automasking_start_normal_falloff;
	float	automasking_view_normal_limit;
	float	automasking_view_normal_falloff;
	CurveMapping	*automasking_cavity_curve;
	CurveMapping	*automasking_cavity_curve_op;
	Object	*gravity_object;
};

struct CurvesSculpt	{
	Paint	paint;
};

struct UvSculpt	{
	CurveMapping	*strength_curve;
	int	size;
	float	strength;
	int8_t	curve_preset;
	char	_pad[7];
};

struct GpPaint	{
	Paint	paint;
	int	flag;
	int	mode;
};

struct GpVertexPaint	{
	Paint	paint;
	int	flag;
	char	_pad[4];
};

struct GpSculptPaint	{
	Paint	paint;
	int	flag;
	char	_pad[4];
};

struct GpWeightPaint	{
	Paint	paint;
	int	flag;
	char	_pad[4];
};

struct VPaint	{
	Paint	paint;
	char	flag;
	char	_pad[3];
	int	radial_symm[3];
};

struct GP_Sculpt_Guide	{
	char	use_guide;
	char	use_snapping;
	char	reference_point;
	char	type;
	char	_pad2[4];
	float	angle;
	float	angle_snap;
	float	spacing;
	float	location[3];
	Object	*reference_object;
};

struct GP_Sculpt_Settings	{
	void	*paintcursor;
	int	flag;
	int	lock_axis;
	float	isect_threshold;
	char	_pad[4];
	CurveMapping	*cur_falloff;
	CurveMapping	*cur_primitive;
	GP_Sculpt_Guide	guide;
};

struct GP_Interpolate_Settings	{
	CurveMapping	*custom_ipo;
};

struct UnifiedPaintSettings	{
	int	size;
	float	unprojected_radius;
	float	alpha;
	float	weight;
	float	rgb[3];
	float	secondary_rgb[3];
	int	color_jitter_flag;
	float	hsv_jitter[3];
	CurveMapping	*curve_rand_hue;
	CurveMapping	*curve_rand_saturation;
	CurveMapping	*curve_rand_value;
	int	input_samples;
	int	flag;
	char	_pad[4];
	float	last_rake[2];
	float	last_rake_angle;
	int	last_stroke_valid;
	float	average_stroke_accum[3];
	int	average_stroke_counter;
	float	brush_rotation;
	float	brush_rotation_sec;
	int	anchored_size;
	float	overlap_factor;
	char	draw_inverted;
	char	stroke_active;
	char	draw_anchored;
	char	do_linear_conversion;
	float	last_location[3];
	int	last_hit;
	float	anchored_initial_mouse[2];
	float	pixel_radius;
	float	initial_pixel_radius;
	float	start_pixel_radius;
	float	size_pressure_value;
	float	tex_mouse[2];
	float	mask_tex_mouse[2];
	void	*colorspace;
};

struct CurvePaintSettings	{
	char	curve_type;
	char	flag;
	char	depth_mode;
	char	surface_plane;
	char	fit_method;
	char	_pad;
	short	error_threshold;
	float	radius_min;
	float	radius_max;
	float	radius_taper_start;
	float	radius_taper_end;
	float	surface_offset;
	float	corner_angle;
};

struct MeshStatVis	{
	char	type;
	char	_pad1[2];
	char	overhang_axis;
	float	overhang_min;
	float	overhang_max;
	float	thickness_min;
	float	thickness_max;
	char	thickness_samples;
	char	_pad2[3];
	float	distort_min;
	float	distort_max;
	float	sharp_min;
	float	sharp_max;
};

struct SequencerToolSettings	{
	int	fit_method;
	short	snap_mode;
	short	snap_flag;
	int	overlap_mode;
	int	snap_distance;
	int	pivot_point;
};

struct ToolSettings	{
	VPaint	*vpaint;
	VPaint	*wpaint;
	Sculpt	*sculpt;
	UvSculpt	uvsculpt;
	GpPaint	*gp_paint;
	GpVertexPaint	*gp_vertexpaint;
	GpSculptPaint	*gp_sculptpaint;
	GpWeightPaint	*gp_weightpaint;
	CurvesSculpt	*curves_sculpt;
	float	vgroup_weight;
	float	doublimit;
	char	automerge;
	char	object_flag;
	char	selectmode;
	char	unwrapper;
	char	uvcalc_flag;
	char	uv_flag;
	char	uv_selectmode;
	char	uv_sticky;
	float	uvcalc_margin;
	int	uvcalc_iterations;
	float	uvcalc_weight_factor;
	char	uvcalc_weight_group[64];
	short	autoik_chainlen;
	char	gpencil_flags;
	char	gpencil_v3d_align;
	char	gpencil_v2d_align;
	char	annotate_v3d_align;
	short	annotate_thickness;
	float	gpencil_surface_offset;
	char	gpencil_selectmode_edit;
	char	gpencil_selectmode_sculpt;
	char	_pad0[6];
	GP_Sculpt_Settings	gp_sculpt;
	GP_Interpolate_Settings	gp_interpolate;
	ImagePaintSettings	imapaint;
	PaintModeSettings	paint_mode;
	ParticleEditSettings	particle;
	float	proportional_size;
	float	select_thresh;
	short	keying_flag;
	char	autokey_mode;
	char	keyframe_type;
	char	multires_subdiv_type;
	char	edge_mode;
	char	edge_mode_live_unwrap;
	char	transform_pivot_point;
	char	transform_flag;
	char	snap_node_mode;
	short	snap_mode;
	short	snap_uv_mode;
	short	snap_anim_mode;
	short	snap_playhead_mode;
	short	snap_flag;
	short	snap_flag_node;
	short	snap_flag_seq;
	short	snap_flag_anim;
	short	snap_flag_driver;
	short	snap_flag_playhead;
	short	snap_uv_flag;
	char	snap_target;
	char	snap_transform_mode_flag;
	short	snap_face_nearest_steps;
	char	proportional_edit;
	char	prop_mode;
	char	proportional_objects;
	char	proportional_mask;
	char	proportional_action;
	char	proportional_fcurve;
	char	lock_markers;
	char	auto_normalize;
	char	wpaint_lock_relative;
	char	multipaint;
	char	weightuser;
	char	vgroupsubset;
	char	gpencil_selectmode_vertex;
	char	uv_sculpt_settings;
	char	workspace_tool_type;
	char	_pad5[7];
	short	sculpt_paint_settings;
	int	sculpt_paint_unified_size;
	float	sculpt_paint_unified_unprojected_radius;
	float	sculpt_paint_unified_alpha;
	UnifiedPaintSettings	unified_paint_settings;
	CurvePaintSettings	curve_paint_settings;
	MeshStatVis	statvis;
	float	normal_vector[3];
	char	_pad6[4];
	CurveProfile	*custom_bevel_profile_preset;
	SequencerToolSettings	*sequencer_tool_settings;
	short	snap_mode_tools;
	char	plane_axis;
	char	plane_depth;
	char	plane_orient;
	char	use_plane_axis_auto;
	char	_pad7[2];
	float	snap_angle_increment_2d;
	float	snap_angle_increment_2d_precision;
	float	snap_angle_increment_3d;
	float	snap_angle_increment_3d_precision;
	short	snap_step_seconds;
	short	snap_step_frames;
	int	playhead_snap_distance;
};

struct UnitSettings	{
	float	scale_length;
	char	system;
	char	system_rotation;
	short	flag;
	char	length_unit;
	char	mass_unit;
	char	time_unit;
	char	temperature_unit;
	char	_pad[4];
};

struct PhysicsSettings	{
	float	gravity[3];
	int	flag;
	int	quick_cache_step;
	char	_pad0[4];
};

struct DisplaySafeAreas	{
	float	title[2];
	float	action[2];
	float	title_center[2];
	float	action_center[2];
};

struct View3DShading	{
	char	type;
	char	prev_type;
	char	prev_type_wire;
	char	color_type;
	short	flag;
	char	light;
	char	background_type;
	char	cavity_type;
	char	wire_color_type;
	char	use_compositor;
	char	_pad;
	char	studio_light[256];
	char	lookdev_light[256];
	char	matcap[256];
	float	shadow_intensity;
	float	single_color[3];
	float	studiolight_rot_z;
	float	studiolight_background;
	float	studiolight_intensity;
	float	studiolight_blur;
	float	object_outline_color[3];
	float	xray_alpha;
	float	xray_alpha_wire;
	float	cavity_valley_factor;
	float	cavity_ridge_factor;
	float	background_color[3];
	float	curvature_ridge_factor;
	float	curvature_valley_factor;
	int	render_pass;
	char	aov_name[64];
	IDProperty	*prop;
	void	*_pad2;
};

struct SceneDisplay	{
	float	light_direction[3];
	float	shadow_shift;
	float	shadow_focus;
	float	matcap_ssao_distance;
	float	matcap_ssao_attenuation;
	int	matcap_ssao_samples;
	char	viewport_aa;
	char	render_aa;
	char	_pad[6];
	View3DShading	shading;
};

struct RaytraceEEVEE	{
	float	screen_trace_quality;
	float	screen_trace_thickness;
	float	trace_max_roughness;
	int	resolution_scale;
	int	flag;
	int	denoise_stages;
};

struct SceneEEVEE	{
	int	flag;
	int	gi_diffuse_bounces;
	int	gi_cubemap_resolution;
	int	gi_visibility_resolution;
	float	gi_glossy_clamp;
	int	gi_irradiance_pool_size;
	char	_pad0[4];
	int	taa_samples;
	int	taa_render_samples;
	float	volumetric_start;
	float	volumetric_end;
	int	volumetric_tile_size;
	int	volumetric_samples;
	float	volumetric_sample_distribution;
	float	volumetric_light_clamp;
	int	volumetric_shadow_samples;
	int	volumetric_ray_depth;
	float	gtao_distance;
	float	gtao_thickness;
	float	gtao_focus;
	int	gtao_resolution;
	int	fast_gi_step_count;
	int	fast_gi_ray_count;
	float	gtao_quality;
	float	fast_gi_distance;
	float	fast_gi_thickness_near;
	float	fast_gi_thickness_far;
	char	fast_gi_method;
	char	_pad1[3];
	float	bokeh_overblur;
	float	bokeh_max_size;
	float	bokeh_threshold;
	float	bokeh_neighbor_max;
	int	motion_blur_samples;
	int	motion_blur_max;
	int	motion_blur_steps;
	int	motion_blur_position;
	float	motion_blur_shutter;
	float	motion_blur_depth_scale;
	int	shadow_cube_size;
	int	shadow_pool_size;
	int	shadow_ray_count;
	int	shadow_step_count;
	float	shadow_resolution_scale;
	float	clamp_surface_direct;
	float	clamp_surface_indirect;
	float	clamp_volume_direct;
	float	clamp_volume_indirect;
	int	ray_tracing_method;
	RaytraceEEVEE	ray_tracing_options;
	float	overscan;
	float	light_threshold;
};

struct SceneGpencil	{
	float	smaa_threshold;
	float	smaa_threshold_render;
	int	aa_samples;
	char	_pad0[4];
};

struct SceneHydra	{
	int	export_method;
	int	_pad0;
};

struct TransformOrientationSlot	{
	int	type;
	int	index_custom;
	char	flag;
	char	_pad0[7];
};

struct View3DCursor	{
	float	location[3];
	float	rotation_quaternion[4];
	float	rotation_euler[3];
	float	rotation_axis[3];
	float	rotation_angle;
	short	rotation_mode;
	char	_pad[6];
};

struct Editing;
struct Scene	{
	ID	id;
	AnimData	*adt;
	Object	*camera;
	World	*world;
	Scene	*set;
	ListBase	base;
	Base	*basact;
	void	*_pad1;
	View3DCursor	cursor;
	int	lay;
	int	layact;
	char	_pad2[4];
	short	flag;
	char	use_nodes;
	char	_pad3[1];
	bNodeTree	*nodetree;
	Editing	*ed;
	ToolSettings	*toolsettings;
	void	*_pad4;
	DisplaySafeAreas	safe_areas;
	RenderData	r;
	AudioData	audio;
	ListBase	markers;
	ListBase	transform_spaces;
	TransformOrientationSlot	orientation_slots[4];
	void	*sound_scene;
	void	*playback_handle;
	void	*sound_scrub_handle;
	void	*speaker_handles;
	void	*fps_info;
	void	*depsgraph_hash;
	char	_pad7[4];
	int	active_keyingset;
	ListBase	keyingsets;
	UnitSettings	unit;
	bGPdata	*gpd;
	MovieClip	*clip;
	PhysicsSettings	physics_settings;
	void	*_pad8;
	CustomData_MeshMasks	customdata_mask;
	CustomData_MeshMasks	customdata_mask_modal;
	ColorManagedViewSettings	view_settings;
	ColorManagedDisplaySettings	display_settings;
	ColorManagedColorspaceSettings	sequencer_colorspace_settings;
	RigidBodyWorld	*rigidbody_world;
	PreviewImage	*preview;
	ListBase	view_layers;
	Collection	*master_collection;
	IDProperty	*layer_properties;
	int	simulation_frame_start;
	int	simulation_frame_end;
	SceneDisplay	display;
	SceneEEVEE	eevee;
	SceneGpencil	grease_pencil_settings;
	SceneHydra	hydra;
	void	*runtime;
	void	*_pad9;
};

struct ARegion;
struct bScreen	{
	ID	id;
	ListBase	vertbase;
	ListBase	edgebase;
	ListBase	areabase;
	ListBase	regionbase;
	Scene	*scene;
	short	flag;
	short	winid;
	short	redraws_flag;
	char	temp;
	char	state;
	char	do_draw;
	char	do_refresh;
	char	do_draw_gesture;
	char	do_draw_paintcursor;
	char	do_draw_drag;
	char	skip_handling;
	char	scrubbing;
	char	_pad[1];
	ARegion	*active_region;
	void	*animtimer;
	void	*context;
	void	*tool_tip;
	PreviewImage	*preview;
};

struct vec2s	{
	short	x;
	short	y;
};

struct ScrVert	{
	ScrVert	*next;
	ScrVert	*prev;
	ScrVert	*newv;
	vec2s	vec;
	short	flag;
	short	editflag;
};

struct ScrEdge	{
	ScrEdge	*next;
	ScrEdge	*prev;
	ScrVert	*v1;
	ScrVert	*v2;
	short	border;
	short	flag;
	char	_pad[4];
};

struct ScrAreaMap	{
	ListBase	vertbase;
	ListBase	edgebase;
	ListBase	areabase;
};

struct LayoutPanelState	{
	LayoutPanelState	*next;
	LayoutPanelState	*prev;
	char	*idname;
	uchar	flag;
	char	_pad[3];
	int	last_used;
};

struct Panel	{
	Panel	*next;
	Panel	*prev;
	void	*type;
	void	*layout;
	char	panelname[64];
	char	*drawname;
	int	ofsx;
	int	ofsy;
	int	sizex;
	int	sizey;
	int	blocksizex;
	int	blocksizey;
	short	labelofs;
	short	flag;
	short	runtime_flag;
	char	_pad[6];
	int	sortorder;
	void	*activedata;
	ListBase	children;
	ListBase	layout_panel_states;
	int	layout_panel_states_clock;
	char	_pad2[4];
	void	*runtime;
};

struct PanelCategoryStack	{
	PanelCategoryStack	*next;
	PanelCategoryStack	*prev;
	char	idname[64];
};

struct uiList	{
	uiList	*next;
	uiList	*prev;
	void	*type;
	char	list_id[128];
	int	layout_type;
	int	flag;
	int	list_scroll;
	int	list_grip;
	int	list_last_len;
	int	list_last_activei;
	char	filter_byname[128];
	int	filter_flag;
	int	filter_sort_flag;
	IDProperty	*properties;
	void	*dyn_data;
};

struct uiViewState	{
	int	custom_height;
	int	scroll_offset;
};

struct uiViewStateLink	{
	uiViewStateLink	*next;
	uiViewStateLink	*prev;
	char	idname[64];
	uiViewState	state;
};

struct TransformOrientation	{
	TransformOrientation	*next;
	TransformOrientation	*prev;
	char	name[64];
	float	mat[3][3];
	char	_pad[4];
};

struct uiPreview	{
	uiPreview	*next;
	uiPreview	*prev;
	char	preview_id[64];
	short	height;
	short	tag;
	int	id_session_uid;
};

struct ScrGlobalAreaData	{
	short	cur_fixed_height;
	short	size_min;
	short	size_max;
	short	align;
	short	flag;
	char	_pad[2];
};

struct bToolRef;
struct ScrArea_Runtime	{
	bToolRef	*tool;
	char	is_tool_set;
	char	_pad0[7];
};

struct ScrArea	{
	ScrArea	*next;
	ScrArea	*prev;
	ScrVert	*v1;
	ScrVert	*v2;
	ScrVert	*v3;
	ScrVert	*v4;
	bScreen	*full;
	rcti	totrct;
	char	spacetype;
	char	butspacetype;
	short	butspacetype_subtype;
	short	winx;
	short	winy;
	char	headertype;
	char	do_refresh;
	short	flag;
	short	region_active_win;
	char	_pad[2];
	void	*type;
	ScrGlobalAreaData	*global;
	ListBase	spacedata;
	ListBase	regionbase;
	ListBase	handlers;
	ListBase	actionzones;
	ScrArea_Runtime	runtime;
};

struct ARegion	{
	ARegion	*next;
	ARegion	*prev;
	View2D	v2d;
	rcti	winrct;
	short	winx;
	short	winy;
	int	category_scroll;
	short	regiontype;
	short	alignment;
	short	flag;
	short	sizex;
	short	sizey;
	short	overlap;
	short	flagfullscreen;
	char	_pad[2];
	ListBase	panels;
	ListBase	panels_category_active;
	ListBase	ui_lists;
	ListBase	ui_previews;
	ListBase	view_states;
	void	*regiondata;
	void	*runtime;
};

struct AssetShelfSettings	{
	AssetLibraryReference	asset_library_reference;
	ListBase	enabled_catalog_paths;
	char	*active_catalog_path;
	char	search_string[64];
	short	preview_size;
	short	display_flag;
	char	_pad1[4];
};

struct AssetShelf	{
	AssetShelf	*next;
	AssetShelf	*prev;
	char	idname[64];
	void	*type;
	AssetShelfSettings	settings;
	short	preferred_row_count;
	short	instance_flag;
	char	_pad[4];
};

struct RegionAssetShelf	{
	ListBase	shelves;
	AssetShelf	*active_shelf;
};

struct FileHandler	{
	void	*type;
};

struct StripAnim	{
	StripAnim	*next;
	StripAnim	*prev;
	void	*anim;
};

struct StripElem	{
	char	name[256];
	int	orig_width;
	int	orig_height;
	float	orig_fps;
};

struct StripCrop	{
	int	top;
	int	bottom;
	int	left;
	int	right;
};

struct StripTransform	{
	float	xofs;
	float	yofs;
	float	scale_x;
	float	scale_y;
	float	rotation;
	float	origin[2];
	int	filter;
};

struct StripColorBalance	{
	int	method;
	float	lift[3];
	float	gamma[3];
	float	gain[3];
	float	slope[3];
	float	offset[3];
	float	power[3];
	int	flag;
	char	_pad[4];
};

struct StripProxy	{
	char	dir[768];
	char	file[256];
	void	*anim;
	short	tc;
	short	quality;
	short	build_size_flags;
	short	build_tc_flags;
	short	build_flags;
	char	storage;
	char	_pad[5];
};

struct Strip	{
	Strip	*next;
	Strip	*prev;
	int	us;
	int	done;
	int	startstill;
	int	endstill;
	StripElem	*stripdata;
	char	dir[768];
	StripProxy	*proxy;
	StripCrop	*crop;
	StripTransform	*transform;
	StripColorBalance	*color_balance;
	ColorManagedColorspaceSettings	colorspace_settings;
};

struct SeqRetimingHandle	{
	double	strip_frame_index;
	int	flag;
	int	_pad0;
	float	retiming_factor;
	char	_pad1[4];
	double	original_strip_frame_index;
	float	original_retiming_factor;
	char	_pad2[4];
};

struct StripRuntime	{
	SessionUID	session_uid;
};

struct bSound;
struct Sequence	{
	Sequence	*next;
	Sequence	*prev;
	void	*_pad;
	void	*lib;
	char	name[64];
	int	flag;
	int	type;
	int	len;
	float	start;
	float	startofs;
	float	endofs;
	float	startstill;
	float	endstill;
	int	machine;
	int	startdisp;
	int	enddisp;
	float	sat;
	float	mul;
	short	streamindex;
	short	_pad1;
	int	multicam_source;
	int	clip_flag;
	Strip	*strip;
	Ipo	*ipo;
	Scene	*scene;
	Object	*scene_camera;
	MovieClip	*clip;
	Mask	*mask;
	ListBase	anims;
	float	effect_fader;
	float	speed_fader;
	Sequence	*seq1;
	Sequence	*seq2;
	void	*_pad7;
	int	_pad8[2];
	ListBase	seqbase;
	ListBase	channels;
	ListBase	connections;
	bSound	*sound;
	void	*scene_sound;
	float	volume;
	float	pitch;
	float	pan;
	float	strobe;
	float	sound_offset;
	char	_pad4[4];
	void	*effectdata;
	int	anim_startofs;
	int	anim_endofs;
	int	blend_mode;
	float	blend_opacity;
	int8_t	color_tag;
	char	alpha_mode;
	char	_pad2[2];
	int	_pad9;
	int	sfra;
	char	views_format;
	char	_pad3[3];
	Stereo3dFormat	*stereo3d_format;
	IDProperty	*prop;
	IDProperty	*system_properties;
	ListBase	modifiers;
	float	media_playback_rate;
	float	speed_factor;
	SeqRetimingHandle	*retiming_handles;
	int	retiming_handle_num;
	char	_pad6[4];
	StripRuntime	runtime;
};

struct MetaStack	{
	MetaStack	*next;
	MetaStack	*prev;
	ListBase	*oldbasep;
	ListBase	*old_channels;
	Sequence	*parseq;
	int	disp_range[2];
};

struct SeqTimelineChannel	{
	SeqTimelineChannel	*next;
	SeqTimelineChannel	*prev;
	char	name[64];
	int	index;
	int	flag;
};

struct SeqConnection	{
	SeqConnection	*next;
	SeqConnection	*prev;
	Sequence	*seq_ref;
};

struct EditingRuntime	{
	void	*strip_lookup;
	void	*media_presence;
	void	*thumbnail_cache;
	void	*intra_frame_cache;
	void	*source_image_cache;
	void	*final_image_cache;
};

struct Editing	{
	ListBase	*seqbasep;
	ListBase	*displayed_channels;
	void	*_pad0;
	ListBase	seqbase;
	ListBase	metastack;
	ListBase	channels;
	Sequence	*act_seq;
	char	act_imagedir[1024];
	char	act_sounddir[1024];
	char	proxy_dir[1024];
	int	proxy_storage;
	int	over_ofs;
	int	over_cfra;
	int	over_flag;
	rctf	over_border;
	int	show_missing_media_flag;
	int	cache_flag;
	void	*prefetch_job;
	EditingRuntime	runtime;
};

struct WipeVars	{
	float	edgeWidth;
	float	angle;
	short	forward;
	short	wipetype;
};

struct GlowVars	{
	float	fMini;
	float	fClamp;
	float	fBoost;
	float	dDist;
	int	dQuality;
	int	bNoComp;
};

struct TransformVars	{
	float	ScalexIni;
	float	ScaleyIni;
	float	xIni;
	float	yIni;
	float	rotIni;
	int	percent;
	int	interpolation;
	int	uniform_scale;
};

struct SolidColorVars	{
	float	col[3];
	char	_pad[4];
};

struct SpeedControlVars	{
	float	*frameMap;
	float	globalSpeed;
	int	flags;
	int	speed_control_type;
	float	speed_fader;
	float	speed_fader_length;
	float	speed_fader_frame_number;
};

struct GaussianBlurVars	{
	float	size_x;
	float	size_y;
};

struct TextVars	{
	char	text[512];
	VFont	*text_font;
	int	text_blf_id;
	float	text_size;
	float	color[4];
	float	shadow_color[4];
	float	box_color[4];
	float	outline_color[4];
	float	loc[2];
	float	wrap_width;
	float	box_margin;
	float	box_roundness;
	float	shadow_angle;
	float	shadow_offset;
	float	shadow_blur;
	float	outline_width;
	char	flag;
	char	align;
	char	_pad[2];
	int	cursor_offset;
	int	selection_start_offset;
	int	selection_end_offset;
	char	align_y;
	char	anchor_x;
	char	anchor_y;
	char	_pad1;
	void	*runtime;
};

struct ColorMixVars	{
	int	blend_effect;
	float	factor;
};

struct SequenceModifierData	{
	SequenceModifierData	*next;
	SequenceModifierData	*prev;
	int	type;
	int	flag;
	char	name[64];
	int	mask_input_type;
	int	mask_time;
	Sequence	*mask_sequence;
	Mask	*mask_id;
};

struct ColorBalanceModifierData	{
	SequenceModifierData	modifier;
	StripColorBalance	color_balance;
	float	color_multiply;
};

struct CurvesModifierData	{
	SequenceModifierData	modifier;
	CurveMapping	curve_mapping;
};

struct HueCorrectModifierData	{
	SequenceModifierData	modifier;
	CurveMapping	curve_mapping;
};

struct BrightContrastModifierData	{
	SequenceModifierData	modifier;
	float	bright;
	float	contrast;
};

struct SequencerMaskModifierData	{
	SequenceModifierData	modifier;
};

struct WhiteBalanceModifierData	{
	SequenceModifierData	modifier;
	float	white_value[3];
	char	_pad[4];
};

struct SequencerTonemapModifierData	{
	SequenceModifierData	modifier;
	float	key;
	float	offset;
	float	gamma;
	float	intensity;
	float	contrast;
	float	adaptation;
	float	correction;
	int	type;
};

struct EQCurveMappingData	{
	EQCurveMappingData	*next;
	EQCurveMappingData	*prev;
	CurveMapping	curve_mapping;
};

struct SoundEqualizerModifierData	{
	SequenceModifierData	modifier;
	ListBase	graphics;
};

struct ShaderFxData	{
	ShaderFxData	*next;
	ShaderFxData	*prev;
	int	type;
	int	mode;
	char	_pad0[4];
	short	flag;
	short	ui_expand_flag;
	char	name[64];
	char	*error;
};

struct ShaderFxData_Runtime	{
	float	loc[3];
	char	_pad[4];
	void	*fx_sh;
	void	*fx_sh_b;
	void	*fx_sh_c;
};

struct BlurShaderFxData	{
	ShaderFxData	shaderfx;
	float	radius[2];
	int	flag;
	int	samples;
	float	rotation;
	char	_pad[4];
	ShaderFxData_Runtime	runtime;
};

struct ColorizeShaderFxData	{
	ShaderFxData	shaderfx;
	int	mode;
	float	low_color[4];
	float	high_color[4];
	float	factor;
	int	flag;
	char	_pad[4];
	ShaderFxData_Runtime	runtime;
};

struct FlipShaderFxData	{
	ShaderFxData	shaderfx;
	int	flag;
	int	flipmode;
	ShaderFxData_Runtime	runtime;
};

struct GlowShaderFxData	{
	ShaderFxData	shaderfx;
	float	glow_color[4];
	float	select_color[3];
	float	threshold;
	int	flag;
	int	mode;
	float	blur[2];
	int	samples;
	float	rotation;
	int	blend_mode;
	char	_pad[4];
	ShaderFxData_Runtime	runtime;
};

struct PixelShaderFxData	{
	ShaderFxData	shaderfx;
	int	size[3];
	int	flag;
	float	rgba[4];
	ShaderFxData_Runtime	runtime;
};

struct RimShaderFxData	{
	ShaderFxData	shaderfx;
	int	offset[2];
	int	flag;
	float	rim_rgb[3];
	float	mask_rgb[3];
	int	mode;
	int	blur[2];
	int	samples;
	char	_pad[4];
	ShaderFxData_Runtime	runtime;
};

struct ShadowShaderFxData	{
	ShaderFxData	shaderfx;
	Object	*object;
	int	offset[2];
	int	flag;
	float	shadow_rgba[4];
	float	amplitude;
	float	period;
	float	phase;
	int	orientation;
	float	scale[2];
	float	rotation;
	int	blur[2];
	int	samples;
	char	_pad[4];
	ShaderFxData_Runtime	runtime;
};

struct SwirlShaderFxData	{
	ShaderFxData	shaderfx;
	Object	*object;
	int	flag;
	int	radius;
	float	angle;
	int	transparent;
	ShaderFxData_Runtime	runtime;
};

struct WaveShaderFxData	{
	ShaderFxData	shaderfx;
	float	amplitude;
	float	period;
	float	phase;
	int	orientation;
	int	flag;
	char	_pad[4];
	ShaderFxData_Runtime	runtime;
};

struct bSound	{
	ID	id;
	char	name[1024];
	PackedFile	*packedfile;
	void	*handle;
	PackedFile	*newpackedfile;
	Ipo	*ipo;
	float	volume;
	float	attenuation;
	float	pitch;
	float	min_gain;
	float	max_gain;
	float	distance;
	short	flags;
	short	tags;
	char	_pad[4];
	double	offset_time;
	void	*cache;
	void	*waveform;
	void	*playback_handle;
	void	*spinlock;
	int	audio_channels;
	int	samplerate;
};

struct SpaceLink	{
	SpaceLink	*next;
	SpaceLink	*prev;
	ListBase	regionbase;
	char	spacetype;
	char	link_flag;
	char	_pad0[6];
};

struct SpaceInfo	{
	SpaceLink	*next;
	SpaceLink	*prev;
	ListBase	regionbase;
	char	spacetype;
	char	link_flag;
	char	_pad0[6];
	char	rpt_mask;
	char	_pad[7];
};

struct SpaceButs	{
	SpaceLink	*next;
	SpaceLink	*prev;
	ListBase	regionbase;
	char	spacetype;
	char	link_flag;
	char	_pad0[6];
	View2D	v2d;
	short	space_subtype;
	short	mainb;
	short	mainbo;
	short	mainbuser;
	int	visible_tabs;
	short	preview;
	char	flag;
	char	outliner_sync;
	void	*path;
	int	pathflag;
	int	dataicon;
	ID	*pinid;
	void	*texuser;
	void	*runtime;
};

struct SpaceOops	{
	SpaceLink	*next;
	SpaceLink	*prev;
	ListBase	regionbase;
	char	spacetype;
	char	link_flag;
	char	_pad0[6];
	View2D	v2d;
	ListBase	tree;
	void	*treestore;
	char	search_string[64];
	short	flag;
	short	outlinevis;
	short	lib_override_view_mode;
	short	storeflag;
	char	search_flags;
	char	_pad[6];
	char	sync_select_dirty;
	int	filter;
	char	filter_state;
	char	show_restrict_flags;
	short	filter_id_type;
	void	*runtime;
};

struct SpaceGraph_Runtime	{
	char	flag;
	char	_pad[7];
	ListBase	ghost_curves;
};

struct SpaceIpo	{
	SpaceLink	*next;
	SpaceLink	*prev;
	ListBase	regionbase;
	char	spacetype;
	char	link_flag;
	char	_pad0[6];
	View2D	v2d;
	bDopeSheet	*ads;
	short	mode;
	short	autosnap;
	int	flag;
	float	cursorTime;
	float	cursorVal;
	int	around;
	char	_pad[4];
	SpaceGraph_Runtime	runtime;
};

struct SpaceNla	{
	SpaceLink	*next;
	SpaceLink	*prev;
	ListBase	regionbase;
	char	spacetype;
	char	link_flag;
	char	_pad0[6];
	short	autosnap;
	short	flag;
	char	_pad[4];
	bDopeSheet	*ads;
	View2D	v2d;
};

struct SequencerPreviewOverlay	{
	int	flag;
	char	_pad0[4];
};

struct SequencerTimelineOverlay	{
	int	flag;
	char	_pad0[4];
};

struct SequencerCacheOverlay	{
	int	flag;
	char	_pad0[4];
};

struct SpaceSeq	{
	SpaceLink	*next;
	SpaceLink	*prev;
	ListBase	regionbase;
	char	spacetype;
	char	link_flag;
	char	_pad0[6];
	View2D	v2d;
	float	xof;
	float	yof;
	short	mainb;
	short	render_size;
	short	chanshown;
	short	zebra;
	int	flag;
	float	zoom;
	char	view;
	char	overlay_type;
	char	draw_flag;
	char	gizmo_flag;
	char	_pad[4];
	float	cursor[2];
	bGPdata	*gpd;
	SequencerPreviewOverlay	preview_overlay;
	SequencerTimelineOverlay	timeline_overlay;
	SequencerCacheOverlay	cache_overlay;
	char	multiview_eye;
	char	_pad2[7];
	void	*runtime;
};

struct MaskSpaceInfo	{
	Mask	*mask;
	char	draw_flag;
	char	draw_type;
	char	overlay_mode;
	char	_pad3[1];
	float	blend_factor;
};

struct FileSelectParams	{
	char	title[96];
	char	dir[1090];
	char	file[256];
	char	renamefile[256];
	short	rename_flag;
	char	_pad[4];
	ID	*rename_id;
	void	*_pad3;
	char	filter_glob[256];
	char	filter_search[64];
	uint64_t	filter_id;
	int	active_file;
	int	highlight_file;
	int	sel_first;
	int	sel_last;
	short	thumbnail_size;
	short	list_thumbnail_size;
	short	list_column_size;
	short	type;
	short	flag;
	short	sort;
	short	display;
	char	details_flags;
	char	_pad1;
	int	filter;
	short	recursion_level;
	char	_pad2[2];
};

struct FileAssetSelectParams	{
	FileSelectParams	base_params;
	AssetLibraryReference	asset_library_ref;
	short	asset_catalog_visibility;
	char	_pad[6];
	bUUID	catalog_id;
	short	import_type;
	short	import_flags;
	char	_pad2[4];
};

struct FileFolderHistory	{
	void	*next;
	void	*prev;
	char	browse_mode;
	char	_pad[7];
	ListBase	folders_prev;
	ListBase	folders_next;
};

struct wmOperator;
struct SpaceFile	{
	SpaceLink	*next;
	SpaceLink	*prev;
	ListBase	regionbase;
	char	spacetype;
	char	link_flag;
	char	_pad0[6];
	char	browse_mode;
	char	_pad1[1];
	short	tags;
	int	scroll_offset;
	FileSelectParams	*params;
	FileAssetSelectParams	*asset_params;
	void	*_pad2;
	void	*files;
	ListBase	*folders_prev;
	ListBase	*folders_next;
	ListBase	folder_histories;
	wmOperator	*op;
	void	*smoothscroll_timer;
	void	*previews_timer;
	void	*layout;
	short	recentnr;
	short	bookmarknr;
	short	systemnr;
	short	system_bookmarknr;
	void	*runtime;
};

struct SpaceImageOverlay	{
	int	flag;
	float	passepartout_alpha;
};

struct SpaceImage	{
	SpaceLink	*next;
	SpaceLink	*prev;
	ListBase	regionbase;
	char	spacetype;
	char	link_flag;
	char	_pad0[6];
	Image	*image;
	ImageUser	iuser;
	Scopes	scopes;
	Histogram	sample_line_hist;
	bGPdata	*gpd;
	float	cursor[2];
	float	xof;
	float	yof;
	float	zoom;
	float	centx;
	float	centy;
	char	mode;
	char	mode_prev;
	char	pin;
	char	pixel_snap_mode;
	char	lock;
	char	dt_uv;
	char	dt_uvstretch;
	char	around;
	char	gizmo_flag;
	char	grid_shape_source;
	char	_pad1[6];
	int	flag;
	float	uv_opacity;
	float	uv_face_opacity;
	char	_pad2[4];
	float	stretch_opacity;
	int	tile_grid_shape[2];
	int	custom_grid_subdiv[2];
	MaskSpaceInfo	mask_info;
	SpaceImageOverlay	overlay;
};

struct SpaceText	{
	SpaceLink	*next;
	SpaceLink	*prev;
	ListBase	regionbase;
	char	spacetype;
	char	link_flag;
	char	_pad0[6];
	Text	*text;
	int	top;
	int	left;
	char	_pad1[4];
	short	flags;
	short	lheight;
	int	tabnumber;
	char	wordwrap;
	char	doplugins;
	char	showlinenrs;
	char	showsyntax;
	char	line_hlight;
	char	overwrite;
	char	live_edit;
	char	_pad2[1];
	char	findstr[256];
	char	replacestr[256];
	short	margin_column;
	char	_pad3[2];
	void	*runtime;
};

struct Script	{
	ID	id;
	void	*py_draw;
	void	*py_event;
	void	*py_button;
	void	*py_browsercallback;
	void	*py_globaldict;
	int	flags;
	int	lastspace;
	char	scriptname[1024];
	char	scriptarg[256];
};

struct SpaceScript	{
	SpaceLink	*next;
	SpaceLink	*prev;
	ListBase	regionbase;
	char	spacetype;
	char	link_flag;
	char	_pad0[6];
	Script	*script;
	short	flags;
	short	menunr;
	char	_pad1[4];
	void	*but_refs;
};

struct bNodeTreePath	{
	bNodeTreePath	*next;
	bNodeTreePath	*prev;
	bNodeTree	*nodetree;
	bNodeInstanceKey	parent_key;
	char	_pad[4];
	float	view_center[2];
	char	node_name[64];
	char	display_name[64];
};

struct SpaceNodeOverlay	{
	int	flag;
	int	preview_shape;
};

struct SpaceNode	{
	SpaceLink	*next;
	SpaceLink	*prev;
	ListBase	regionbase;
	char	spacetype;
	char	link_flag;
	char	_pad0[6];
	View2D	v2d;
	ID	*id;
	ID	*from;
	short	flag;
	char	insert_ofs_dir;
	char	_pad1;
	float	xof;
	float	yof;
	float	zoom;
	ListBase	treepath;
	bNodeTree	*edittree;
	bNodeTree	*nodetree;
	char	tree_idname[64];
	int	treetype;
	short	texfrom;
	char	shaderfrom;
	char	geometry_nodes_type;
	bNodeTree	*geometry_nodes_tool_tree;
	bGPdata	*gpd;
	char	gizmo_flag;
	char	_pad2[7];
	SpaceNodeOverlay	overlay;
	void	*runtime;
};

struct ConsoleLine	{
	ConsoleLine	*next;
	ConsoleLine	*prev;
	int	len_alloc;
	int	len;
	char	*line;
	int	cursor;
	int	type;
};

struct SpaceConsole	{
	SpaceLink	*next;
	SpaceLink	*prev;
	ListBase	regionbase;
	char	spacetype;
	char	link_flag;
	char	_pad0[6];
	ListBase	scrollback;
	ListBase	history;
	char	prompt[256];
	char	language[32];
	int	lheight;
	int	history_index;
	int	sel_start;
	int	sel_end;
};

struct SpaceUserPref	{
	SpaceLink	*next;
	SpaceLink	*prev;
	ListBase	regionbase;
	char	spacetype;
	char	link_flag;
	char	_pad0[6];
	char	_pad1[7];
	char	filter_type;
	char	filter[64];
};

struct SpaceClip	{
	SpaceLink	*next;
	SpaceLink	*prev;
	ListBase	regionbase;
	char	spacetype;
	char	link_flag;
	char	_pad0[6];
	char	gizmo_flag;
	char	_pad1[3];
	float	xof;
	float	yof;
	float	xlockof;
	float	ylockof;
	float	zoom;
	MovieClipUser	user;
	MovieClip	*clip;
	MovieClipScopes	scopes;
	int	flag;
	short	mode;
	short	view;
	int	path_length;
	float	loc[2];
	float	scale;
	float	angle;
	char	_pad[4];
	float	stabmat[4][4];
	float	unistabmat[4][4];
	int	postproc_flag;
	short	gpencil_src;
	char	_pad2[2];
	int	around;
	char	_pad4[4];
	float	cursor[2];
	MaskSpaceInfo	mask_info;
};

struct SpaceTopBar	{
	SpaceLink	*next;
	SpaceLink	*prev;
	ListBase	regionbase;
	char	spacetype;
	char	link_flag;
	char	_pad0[6];
};

struct SpaceStatusBar	{
	SpaceLink	*next;
	SpaceLink	*prev;
	ListBase	regionbase;
	char	spacetype;
	char	link_flag;
	char	_pad0[6];
};

struct SpreadsheetColumnID	{
	char	*name;
};

struct SpreadsheetColumn	{
	SpreadsheetColumnID	*id;
	uchar	data_type;
	char	_pad0[3];
	int	flag;
	float	width;
	int	last_used;
	char	*display_name;
	void	*runtime;
};

struct SpreadsheetInstanceID	{
	int	reference_index;
};

struct SpreadsheetTableID	{
	int	type;
};

struct ViewerPath	{
	ListBase	path;
};

struct SpreadsheetTableIDGeometry	{
	SpreadsheetTableID	base;
	char	_pad0[4];
	ViewerPath	viewer_path;
	SpreadsheetInstanceID	*instance_ids;
	int	instance_ids_num;
	uchar	geometry_component_type;
	uchar	attribute_domain;
	uchar	object_eval_state;
	char	_pad1[5];
	int	layer_index;
};

struct SpreadsheetTable	{
	SpreadsheetTableID	*id;
	SpreadsheetColumn	**columns;
	int	num_columns;
	int	flag;
	int	last_used;
	int	column_use_clock;
};

struct SpaceSpreadsheet	{
	SpaceLink	*next;
	SpaceLink	*prev;
	ListBase	regionbase;
	char	spacetype;
	char	link_flag;
	char	_pad0[6];
	SpreadsheetTable	**tables;
	int	num_tables;
	char	_pad1[3];
	uchar	filter_flag;
	ListBase	row_filters;
	SpreadsheetTableIDGeometry	geometry_id;
	int	flag;
	int	table_use_clock;
	int	active_viewer_path_index;
	char	_pad2[4];
	void	*runtime;
};

struct SpreadsheetRowFilter	{
	SpreadsheetRowFilter	*next;
	SpreadsheetRowFilter	*prev;
	char	column_name[64];
	uchar	operation;
	uchar	flag;
	char	_pad0[2];
	int	value_int;
	int	value_int2[2];
	char	*value_string;
	float	value_float;
	float	threshold;
	float	value_float2[2];
	float	value_float3[3];
	float	value_color[4];
	char	_pad1[4];
};

struct Speaker	{
	ID	id;
	AnimData	*adt;
	bSound	*sound;
	float	volume_max;
	float	volume_min;
	float	distance_max;
	float	distance_reference;
	float	attenuation;
	float	cone_angle_outer;
	float	cone_angle_inner;
	float	cone_volume_outer;
	float	volume;
	float	pitch;
	short	flag;
	char	_pad1[6];
};

struct TextLine	{
	TextLine	*next;
	TextLine	*prev;
	char	*line;
	char	*format;
	int	len;
	char	_pad0[4];
};

struct Text	{
	ID	id;
	char	*name;
	void	*compiled;
	int	flags;
	char	_pad0[4];
	ListBase	lines;
	TextLine	*curl;
	TextLine	*sell;
	int	curc;
	int	selc;
	double	mtime;
};

struct Tex_Runtime	{
	uint64_t	last_update;
};

struct Tex	{
	ID	id;
	AnimData	*adt;
	float	noisesize;
	float	turbul;
	float	bright;
	float	contrast;
	float	saturation;
	float	rfac;
	float	gfac;
	float	bfac;
	float	filtersize;
	char	_pad2[4];
	float	mg_H;
	float	mg_lacunarity;
	float	mg_octaves;
	float	mg_offset;
	float	mg_gain;
	float	dist_amount;
	float	ns_outscale;
	float	vn_w1;
	float	vn_w2;
	float	vn_w3;
	float	vn_w4;
	float	vn_mexp;
	short	vn_distm;
	short	vn_coltype;
	short	noisedepth;
	short	noisetype;
	short	noisebasis;
	short	noisebasis2;
	short	imaflag;
	short	flag;
	short	type;
	short	stype;
	float	cropxmin;
	float	cropymin;
	float	cropxmax;
	float	cropymax;
	int	texfilter;
	int	afmax;
	short	xrepeat;
	short	yrepeat;
	short	extend;
	short	_pad0;
	int	len;
	int	frames;
	int	offset;
	int	sfra;
	float	checkerdist;
	float	nabla;
	char	_pad1[4];
	ImageUser	iuser;
	bNodeTree	*nodetree;
	Ipo	*ipo;
	Image	*ima;
	ColorBand	*coba;
	PreviewImage	*preview;
	char	use_nodes;
	char	_pad[7];
	void	*_pad3;
	Tex_Runtime	runtime;
};

struct uiFontStyle	{
	short	uifont_id;
	char	_pad1[2];
	float	points;
	short	italic;
	short	bold;
	short	shadow;
	short	shadx;
	short	shady;
	char	_pad0[2];
	float	shadowalpha;
	float	shadowcolor;
	int	character_weight;
};

struct uiStyle	{
	uiStyle	*next;
	uiStyle	*prev;
	char	name[64];
	uiFontStyle	paneltitle;
	uiFontStyle	grouplabel;
	uiFontStyle	widget;
	uiFontStyle	tooltip;
	float	panelzoom;
	short	minlabelchars;
	short	minwidgetchars;
	short	columnspace;
	short	templatespace;
	short	boxspace;
	short	buttonspacex;
	short	buttonspacey;
	short	panelspace;
	short	panelouter;
	char	_pad0[2];
};

struct uiWidgetColors	{
	char	outline[4];
	char	inner[4];
	char	inner_sel[4];
	char	item[4];
	char	text[4];
	char	text_sel[4];
	char	shaded;
	char	_pad0[7];
	short	shadetop;
	short	shadedown;
	float	roundness;
};

struct uiWidgetStateColors	{
	char	error[4];
	char	warning[4];
	char	info[4];
	char	success[4];
	char	inner_anim[4];
	char	inner_anim_sel[4];
	char	inner_key[4];
	char	inner_key_sel[4];
	char	inner_driven[4];
	char	inner_driven_sel[4];
	char	inner_overridden[4];
	char	inner_overridden_sel[4];
	char	inner_changed[4];
	char	inner_changed_sel[4];
	float	blend;
	char	_pad0[4];
};

struct uiPanelColors	{
	char	header[4];
	char	back[4];
	char	sub_back[4];
	char	_pad0[4];
};

struct ThemeUI	{
	uiWidgetColors	wcol_regular;
	uiWidgetColors	wcol_tool;
	uiWidgetColors	wcol_toolbar_item;
	uiWidgetColors	wcol_text;
	uiWidgetColors	wcol_radio;
	uiWidgetColors	wcol_option;
	uiWidgetColors	wcol_toggle;
	uiWidgetColors	wcol_num;
	uiWidgetColors	wcol_numslider;
	uiWidgetColors	wcol_tab;
	uiWidgetColors	wcol_menu;
	uiWidgetColors	wcol_pulldown;
	uiWidgetColors	wcol_menu_back;
	uiWidgetColors	wcol_menu_item;
	uiWidgetColors	wcol_tooltip;
	uiWidgetColors	wcol_box;
	uiWidgetColors	wcol_scroll;
	uiWidgetColors	wcol_progress;
	uiWidgetColors	wcol_list_item;
	uiWidgetColors	wcol_pie_menu;
	uiWidgetStateColors	wcol_state;
	char	widget_emboss[4];
	float	menu_shadow_fac;
	short	menu_shadow_width;
	char	editor_border[4];
	char	editor_outline[4];
	char	editor_outline_active[4];
	char	transparent_checker_primary[4];
	char	transparent_checker_secondary[4];
	char	transparent_checker_size;
	char	_pad1[1];
	float	icon_alpha;
	float	icon_saturation;
	char	widget_text_cursor[4];
	char	xaxis[4];
	char	yaxis[4];
	char	zaxis[4];
	char	gizmo_hi[4];
	char	gizmo_primary[4];
	char	gizmo_secondary[4];
	char	gizmo_view_align[4];
	char	gizmo_a[4];
	char	gizmo_b[4];
	char	icon_scene[4];
	char	icon_collection[4];
	char	icon_object[4];
	char	icon_object_data[4];
	char	icon_modifier[4];
	char	icon_shading[4];
	char	icon_folder[4];
	char	icon_autokey[4];
	char	_pad3[4];
	float	icon_border_intensity;
	float	panel_roundness;
	char	_pad2[4];
};

struct ThemeAssetShelf	{
	char	header_back[4];
	char	back[4];
};

struct ThemeSpace	{
	char	back[4];
	char	back_grad[4];
	char	show_back_grad;
	char	_pad0[3];
	char	title[4];
	char	text[4];
	char	text_hi[4];
	char	header[4];
	char	header_title[4];
	char	header_text[4];
	char	header_text_hi[4];
	char	tab_active[4];
	char	tab_inactive[4];
	char	tab_back[4];
	char	tab_outline[4];
	char	button[4];
	char	button_title[4];
	char	button_text[4];
	char	button_text_hi[4];
	char	list[4];
	char	list_title[4];
	char	list_text[4];
	char	list_text_hi[4];
	char	navigation_bar[4];
	char	execution_buts[4];
	uiPanelColors	panelcolors;
	ThemeAssetShelf	asset_shelf;
	char	shade1[4];
	char	shade2[4];
	char	hilite[4];
	char	grid[4];
	char	view_overlay[4];
	char	wire[4];
	char	wire_edit[4];
	char	select[4];
	char	lamp[4];
	char	speaker[4];
	char	empty[4];
	char	camera[4];
	char	active[4];
	char	group[4];
	char	group_active[4];
	char	transform[4];
	char	vertex[4];
	char	vertex_select[4];
	char	vertex_active[4];
	char	vertex_bevel[4];
	char	vertex_unreferenced[4];
	char	edge[4];
	char	edge_select[4];
	char	edge_mode_select[4];
	char	edge_seam[4];
	char	edge_sharp[4];
	char	edge_facesel[4];
	char	edge_crease[4];
	char	edge_bevel[4];
	char	face[4];
	char	face_select[4];
	char	face_mode_select[4];
	char	face_retopology[4];
	char	face_back[4];
	char	face_front[4];
	char	face_dot[4];
	char	extra_edge_len[4];
	char	extra_edge_angle[4];
	char	extra_face_angle[4];
	char	extra_face_area[4];
	char	normal[4];
	char	vertex_normal[4];
	char	loop_normal[4];
	char	bone_solid[4];
	char	bone_pose[4];
	char	bone_pose_active[4];
	char	bone_locked_weight[4];
	char	strip[4];
	char	strip_select[4];
	char	cframe[4];
	char	before_current_frame[4];
	char	after_current_frame[4];
	char	time_keyframe[4];
	char	time_gp_keyframe[4];
	char	freestyle_edge_mark[4];
	char	freestyle_face_mark[4];
	char	scrubbing_background[4];
	char	time_marker_line[4];
	char	time_marker_line_selected[4];
	char	nurb_uline[4];
	char	nurb_vline[4];
	char	act_spline[4];
	char	nurb_sel_uline[4];
	char	nurb_sel_vline[4];
	char	lastsel_point[4];
	char	handle_free[4];
	char	handle_auto[4];
	char	handle_vect[4];
	char	handle_align[4];
	char	handle_auto_clamped[4];
	char	handle_sel_free[4];
	char	handle_sel_auto[4];
	char	handle_sel_vect[4];
	char	handle_sel_align[4];
	char	handle_sel_auto_clamped[4];
	char	ds_channel[4];
	char	ds_subchannel[4];
	char	ds_ipoline[4];
	char	keytype_keyframe[4];
	char	keytype_extreme[4];
	char	keytype_breakdown[4];
	char	keytype_jitter[4];
	char	keytype_movehold[4];
	char	keytype_generated[4];
	char	keytype_keyframe_select[4];
	char	keytype_extreme_select[4];
	char	keytype_breakdown_select[4];
	char	keytype_jitter_select[4];
	char	keytype_movehold_select[4];
	char	keytype_generated_select[4];
	char	keyborder[4];
	char	keyborder_select[4];
	char	_pad4[3];
	char	console_output[4];
	char	console_input[4];
	char	console_info[4];
	char	console_error[4];
	char	console_cursor[4];
	char	console_select[4];
	char	vertex_size;
	char	edge_width;
	char	outline_width;
	char	obcenter_dia;
	char	facedot_size;
	char	noodle_curving;
	char	grid_levels;
	char	_pad5[2];
	float	dash_alpha;
	char	syntaxl[4];
	char	syntaxs[4];
	char	syntaxb[4];
	char	syntaxn[4];
	char	syntaxv[4];
	char	syntaxc[4];
	char	syntaxd[4];
	char	syntaxr[4];
	char	line_numbers[4];
	char	nodeclass_output[4];
	char	nodeclass_filter[4];
	char	nodeclass_vector[4];
	char	nodeclass_texture[4];
	char	nodeclass_shader[4];
	char	nodeclass_script[4];
	char	nodeclass_pattern[4];
	char	nodeclass_layout[4];
	char	nodeclass_geometry[4];
	char	nodeclass_attribute[4];
	char	node_zone_simulation[4];
	char	node_zone_repeat[4];
	char	node_zone_foreach_geometry_element[4];
	char	node_zone_closure[4];
	char	simulated_frames[4];
	char	_pad7[4];
	char	movie[4];
	char	movieclip[4];
	char	mask[4];
	char	image[4];
	char	scene[4];
	char	audio[4];
	char	effect[4];
	char	transition[4];
	char	meta[4];
	char	text_strip[4];
	char	color_strip[4];
	char	active_strip[4];
	char	selected_strip[4];
	char	text_strip_cursor[4];
	char	selected_text[4];
	float	keyframe_scale_fac;
	char	editmesh_active[4];
	char	handle_vertex[4];
	char	handle_vertex_select[4];
	char	handle_vertex_size;
	char	clipping_border_3d[4];
	char	marker_outline[4];
	char	marker[4];
	char	act_marker[4];
	char	sel_marker[4];
	char	dis_marker[4];
	char	lock_marker[4];
	char	bundle_solid[4];
	char	path_before[4];
	char	path_after[4];
	char	path_keyframe_before[4];
	char	path_keyframe_after[4];
	char	camera_path[4];
	char	camera_passepartout[4];
	char	_pad1[2];
	char	gp_vertex_size;
	char	gp_vertex[4];
	char	gp_vertex_select[4];
	char	preview_back[4];
	char	preview_stitch_face[4];
	char	preview_stitch_edge[4];
	char	preview_stitch_vert[4];
	char	preview_stitch_stitchable[4];
	char	preview_stitch_unstitchable[4];
	char	preview_stitch_active[4];
	char	uv_shadow[4];
	char	match[4];
	char	selected_highlight[4];
	char	selected_object[4];
	char	active_object[4];
	char	edited_object[4];
	char	row_alternate[4];
	char	skin_root[4];
	char	anim_active[4];
	char	anim_non_active[4];
	char	anim_preview_range[4];
	char	nla_tweaking[4];
	char	nla_tweakdupli[4];
	char	nla_track[4];
	char	nla_transition[4];
	char	nla_transition_sel[4];
	char	nla_meta[4];
	char	nla_meta_sel[4];
	char	nla_sound[4];
	char	nla_sound_sel[4];
	char	info_selected[4];
	char	info_selected_text[4];
	char	info_error_text[4];
	char	info_warning_text[4];
	char	info_info_text[4];
	char	info_debug[4];
	char	info_debug_text[4];
	char	info_property[4];
	char	info_property_text[4];
	char	info_operator[4];
	char	info_operator_text[4];
	char	_pad6[4];
	char	paint_curve_pivot[4];
	char	paint_curve_handle[4];
	char	metadatabg[4];
	char	metadatatext[4];
};

struct ThemeCollectionColor	{
	char	color[4];
};

struct ThemeStripColor	{
	char	color[4];
};

struct bTheme	{
	bTheme	*next;
	bTheme	*prev;
	char	name[64];
	char	filepath[1024];
	ThemeUI	tui;
	ThemeSpace	tbuts;
	ThemeSpace	tv3d;
	ThemeSpace	tfile;
	ThemeSpace	tipo;
	ThemeSpace	tinfo;
	ThemeSpace	tact;
	ThemeSpace	tnla;
	ThemeSpace	tseq;
	ThemeSpace	tima;
	ThemeSpace	text;
	ThemeSpace	toops;
	ThemeSpace	tnode;
	ThemeSpace	tuserpref;
	ThemeSpace	tconsole;
	ThemeSpace	tclip;
	ThemeSpace	ttopbar;
	ThemeSpace	tstatusbar;
	ThemeSpace	space_spreadsheet;
	ThemeWireColor	tarm[20];
	ThemeCollectionColor	collection_color[8];
	ThemeStripColor	strip_color[9];
	int	active_theme_area;
};

struct MovieReconstructedCamera	{
	int	framenr;
	float	error;
	float	mat[4][4];
};

struct MovieTrackingTrack	{
	MovieTrackingTrack	*next;
	MovieTrackingTrack	*prev;
	char	name[64];
	float	pat_min[2];
	float	pat_max[2];
	float	search_min[2];
	float	search_max[2];
	float	offset[2];
	int	markersnr;
	int	_pad;
	MovieTrackingMarker	*markers;
	float	bundle_pos[3];
	float	error;
	int	flag;
	int	pat_flag;
	int	search_flag;
	float	color[3];
	short	frames_limit;
	short	margin;
	short	pattern_match;
	short	motion_model;
	int	algorithm_flag;
	float	minimum_correlation;
	bGPdata	*gpd;
	float	weight;
	float	weight_stab;
};

struct MovieTrackingPlaneMarker	{
	float	corners[4][2];
	int	framenr;
	int	flag;
};

struct MovieTrackingPlaneTrack	{
	MovieTrackingPlaneTrack	*next;
	MovieTrackingPlaneTrack	*prev;
	char	name[64];
	MovieTrackingTrack	**point_tracks;
	int	point_tracksnr;
	char	_pad[4];
	MovieTrackingPlaneMarker	*markers;
	int	markersnr;
	int	flag;
	Image	*image;
	float	image_opacity;
	int	last_marker;
};

struct MovieTrackingObject	{
	MovieTrackingObject	*next;
	MovieTrackingObject	*prev;
	char	name[64];
	int	flag;
	float	scale;
	ListBase	tracks;
	ListBase	plane_tracks;
	MovieTrackingTrack	*active_track;
	MovieTrackingPlaneTrack	*active_plane_track;
	MovieTrackingReconstruction	reconstruction;
	int	keyframe1;
	int	keyframe2;
};

struct MovieTrackingStats	{
	char	message[256];
};

struct MovieTrackingDopesheetChannel	{
	MovieTrackingDopesheetChannel	*next;
	MovieTrackingDopesheetChannel	*prev;
	MovieTrackingTrack	*track;
	char	_pad[4];
	char	name[64];
	int	tot_segment;
	int	*segments;
	int	max_segment;
	int	total_frames;
	int	first_not_disabled_marker_framenr;
	int	last_not_disabled_marker_framenr;
};

struct MovieTrackingDopesheetCoverageSegment	{
	MovieTrackingDopesheetCoverageSegment	*next;
	MovieTrackingDopesheetCoverageSegment	*prev;
	int	coverage;
	int	start_frame;
	int	end_frame;
	char	_pad[4];
};

struct bAddon	{
	bAddon	*next;
	bAddon	*prev;
	char	module[128];
	IDProperty	*prop;
};

struct bPathCompare	{
	bPathCompare	*next;
	bPathCompare	*prev;
	char	path[768];
	char	flag;
	char	_pad0[7];
};

struct bUserMenu	{
	bUserMenu	*next;
	bUserMenu	*prev;
	char	space_type;
	char	_pad0[7];
	char	context[64];
	ListBase	items;
};

struct bUserMenuItem	{
	bUserMenuItem	*next;
	bUserMenuItem	*prev;
	char	ui_name[64];
	char	type;
	char	_pad0[7];
};

struct bUserMenuItem_Op	{
	bUserMenuItem	item;
	char	op_idname[64];
	IDProperty	*prop;
	char	op_prop_enum[64];
	char	opcontext;
	char	_pad0[7];
};

struct bUserMenuItem_Menu	{
	bUserMenuItem	item;
	char	mt_idname[64];
};

struct bUserMenuItem_Prop	{
	bUserMenuItem	item;
	char	context_data_path[256];
	char	prop_id[64];
	int	prop_index;
	char	_pad0[4];
};

struct bUserAssetLibrary	{
	bUserAssetLibrary	*next;
	bUserAssetLibrary	*prev;
	char	name[64];
	char	path[1024];
	short	import_method;
	short	flag;
	char	_pad0[4];
};

struct bUserExtensionRepo	{
	bUserExtensionRepo	*next;
	bUserExtensionRepo	*prev;
	char	name[64];
	char	module[48];
	char	*access_token;
	char	custom_dirpath[1024];
	char	remote_url[1024];
	uchar	flag;
	uchar	source;
	char	_pad0[6];
};

struct SolidLight	{
	int	flag;
	float	smooth;
	float	col[4];
	float	spec[4];
	float	vec[4];
};

struct WalkNavigation	{
	float	mouse_speed;
	float	walk_speed;
	float	walk_speed_factor;
	float	view_height;
	float	jump_height;
	float	teleport_time;
	short	flag;
	char	_pad0[6];
};

struct UserDef_Runtime	{
	char	is_dirty;
	char	_pad0[7];
};

struct UserDef_SpaceData	{
	char	section_active;
	char	flag;
	char	_pad0[6];
};

struct UserDef_FileSpaceData	{
	int	display_type;
	int	thumbnail_size;
	int	sort_type;
	int	details_flags;
	int	flag;
	int	_pad0;
	uint64_t	filter_id;
	int	temp_win_sizex;
	int	temp_win_sizey;
};

struct UserDef_Experimental	{
	char	use_undo_legacy;
	char	no_override_auto_resync;
	char	use_cycles_debug;
	char	use_eevee_debug;
	char	show_asset_debug_info;
	char	no_asset_indexing;
	char	use_viewport_debug;
	char	use_all_linked_data_direct;
	char	use_extensions_debug;
	char	use_recompute_usercount_on_save_debug;
	char	write_large_blend_file_blocks;
	char	use_attribute_storage_write;
	char	SANITIZE_AFTER_HERE;
	char	use_new_curves_tools;
	char	use_extended_asset_browser;
	char	use_sculpt_texture_paint;
	char	use_new_volume_nodes;
	char	use_shader_node_previews;
	char	use_bundle_and_closure_nodes;
	char	use_socket_structure_type;
	char	_pad[4];
};

struct bUserScriptDirectory	{
	bUserScriptDirectory	*next;
	bUserScriptDirectory	*prev;
	char	name[64];
	char	dir_path[768];
};

struct bUserAssetShelfSettings	{
	bUserAssetShelfSettings	*next;
	bUserAssetShelfSettings	*prev;
	char	shelf_idname[64];
	ListBase	enabled_catalog_paths;
};

struct UserDef	{
	int	versionfile;
	int	subversionfile;
	int	flag;
	int	dupflag;
	char	pref_flag;
	char	savetime;
	char	mouse_emulate_3_button_modifier;
	char	trackpad_scroll_direction;
	char	tempdir[768];
	char	fontdir[768];
	char	renderdir[1024];
	char	render_cachedir[768];
	char	textudir[768];
	char	pythondir[768];
	char	sounddir[768];
	char	i18ndir[768];
	char	image_editor[1024];
	char	text_editor[1024];
	char	text_editor_args[256];
	char	anim_player[1024];
	int	anim_player_preset;
	short	v2d_min_gridsize;
	short	timecode_style;
	short	versions;
	short	dbl_click_time;
	char	_pad0[2];
	char	border_width;
	char	mini_axis_type;
	int	uiflag;
	char	uiflag2;
	char	gpu_flag;
	char	_pad8[6];
	char	app_flag;
	char	viewzoom;
	short	language;
	int	mixbufsize;
	int	audiodevice;
	int	audiorate;
	int	audioformat;
	int	audiochannels;
	float	ui_scale;
	int	ui_line_width;
	int	dpi;
	float	scale_factor;
	float	inv_scale_factor;
	float	pixelsize;
	int	virtual_pixel;
	int	scrollback;
	char	node_margin;
	char	node_preview_res;
	short	transopts;
	short	menuthreshold1;
	short	menuthreshold2;
	char	app_template[64];
	ListBase	themes;
	ListBase	uifonts;
	ListBase	uistyles;
	ListBase	user_keymaps;
	ListBase	user_keyconfig_prefs;
	ListBase	addons;
	ListBase	autoexec_paths;
	ListBase	script_directories;
	ListBase	user_menus;
	ListBase	asset_libraries;
	ListBase	extension_repos;
	ListBase	asset_shelves_settings;
	char	keyconfigstr[64];
	short	active_asset_library;
	short	active_extension_repo;
	char	extension_flag;
	uchar	network_timeout;
	uchar	network_connection_limit;
	char	_pad14[3];
	short	undosteps;
	int	undomemory;
	float	gpu_viewport_quality;
	short	gp_manhattendist;
	short	gp_euclideandist;
	short	gp_eraser;
	short	gp_settings;
	char	_pad13[4];
	SolidLight	light_param[4];
	float	light_ambient[3];
	char	gizmo_flag;
	char	gizmo_size;
	char	gizmo_size_navigate_v3d;
	char	_pad3[5];
	short	edit_studio_light;
	short	lookdev_sphere_size;
	short	vbotimeout;
	short	vbocollectrate;
	short	textimeout;
	short	texcollectrate;
	int	memcachelimit;
	int	prefetchframes;
	float	pad_rot_angle;
	char	_pad12[4];
	short	rvisize;
	short	rvibright;
	short	recent_files;
	short	smooth_viewtx;
	short	glreslimit;
	short	color_picker_type;
	char	auto_smoothing_new;
	char	ipo_new;
	char	keyhandles_new;
	char	_pad11[4];
	char	view_frame_type;
	int	view_frame_keyframes;
	float	view_frame_seconds;
	int	gpu_preferred_index;
	int	gpu_preferred_vendor_id;
	int	gpu_preferred_device_id;
	short	max_shader_compilation_subprocesses;
	short	shader_compilation_method;
	char	_pad16[2];
	short	gpu_backend;
	short	playback_fps_samples;
	short	widget_unit;
	short	anisotropic_filter;
	short	tablet_api;
	float	pressure_threshold_max;
	float	pressure_softness;
	float	ndof_sensitivity;
	float	ndof_orbit_sensitivity;
	float	ndof_deadzone;
	int	ndof_flag;
	uchar	ndof_navigation_mode;
	char	_pad17[1];
	short	image_draw_method;
	float	glalphaclip;
	short	autokey_mode;
	short	autokey_flag;
	short	key_insert_channels;
	char	_pad15[6];
	short	animation_flag;
	char	text_render;
	char	navigation_mode;
	float	view_rotate_sensitivity_turntable;
	float	view_rotate_sensitivity_trackball;
	ColorBand	coba_weight;
	float	sculpt_paint_overlay_col[3];
	float	gpencil_new_layer_col[4];
	char	drag_threshold_mouse;
	char	drag_threshold_tablet;
	char	drag_threshold;
	char	move_threshold;
	char	font_path_ui[1024];
	char	font_path_ui_mono[1024];
	int	compute_device_type;
	float	fcu_inactive_alpha;
	short	pie_tap_timeout;
	short	pie_initial_timeout;
	short	pie_animation_timeout;
	short	pie_menu_confirm;
	short	pie_menu_radius;
	short	pie_menu_threshold;
	int	sequencer_editor_flag;
	char	factor_display_type;
	char	viewport_aa;
	char	render_display_type;
	char	filebrowser_display_type;
	short	sequencer_proxy_setup;
	short	_pad1;
	float	collection_instance_empty_size;
	char	text_flag;
	char	_pad10[1];
	char	file_preview_type;
	char	statusbar_flag;
	WalkNavigation	walk_navigation;
	UserDef_SpaceData	space_data;
	UserDef_FileSpaceData	file_space_data;
	UserDef_Experimental	experimental;
	UserDef_Runtime	runtime;
};

struct vec2f	{
	float	x;
	float	y;
};

struct vec2i	{
	int	x;
	int	y;
};

struct vec3i	{
	int	x;
	int	y;
	int	z;
};

struct vec3f	{
	float	x;
	float	y;
	float	z;
};

struct vec4f	{
	float	x;
	float	y;
	float	z;
	float	w;
};

struct mat4x4f	{
	float	value[4][4];
};

struct VFont	{
	ID	id;
	char	name[1024];
	void	*data;
	PackedFile	*packedfile;
	PackedFile	*temp_pf;
};

struct RegionView3D	{
	float	winmat[4][4];
	float	viewmat[4][4];
	float	viewinv[4][4];
	float	persmat[4][4];
	float	persinv[4][4];
	float	viewcamtexcofac[4];
	float	viewmatob[4][4];
	float	persmatob[4][4];
	float	clip[6][4];
	float	clip_local[6][4];
	BoundBox	*clipbb;
	RegionView3D	*localvd;
	void	*view_render;
	void	*sms;
	void	*smooth_timer;
	float	twmat[4][4];
	float	tw_axis_min[3];
	float	tw_axis_max[3];
	float	tw_axis_matrix[3][3];
	float	gridview;
	float	viewquat[4];
	float	dist;
	float	camdx;
	float	camdy;
	float	pixsize;
	float	ofs[3];
	float	camzoom;
	char	is_persp;
	char	persp;
	char	view;
	char	view_axis_roll;
	char	viewlock;
	char	runtime_viewlock;
	char	viewlock_quad;
	char	_pad[1];
	float	ofs_lock[2];
	short	twdrawflag;
	short	rflag;
	float	lviewquat[4];
	char	lpersp;
	char	lview;
	char	lview_axis_roll;
	char	_pad8[4];
	char	ndof_flag;
	float	ndof_ofs[3];
	float	ndof_rot_angle;
	float	ndof_rot_axis[3];
};

struct View3DOverlay	{
	int	flag;
	int	edit_flag;
	float	normals_length;
	float	normals_constant_screen_size;
	int	paint_flag;
	int	wpaint_flag;
	float	texture_paint_mode_opacity;
	float	vertex_paint_mode_opacity;
	float	weight_paint_mode_opacity;
	float	sculpt_mode_mask_opacity;
	float	sculpt_mode_face_sets_opacity;
	float	viewer_attribute_opacity;
	float	xray_alpha_bone;
	float	bone_wire_alpha;
	float	fade_alpha;
	float	wireframe_threshold;
	float	wireframe_opacity;
	float	retopology_offset;
	float	gpencil_paper_opacity;
	float	gpencil_grid_opacity;
	float	gpencil_fade_layer;
	float	gpencil_grid_color[3];
	float	gpencil_grid_scale[2];
	float	gpencil_grid_offset[2];
	int	gpencil_grid_subdivisions;
	float	gpencil_vertex_paint_opacity;
	int	handle_display;
	float	sculpt_curves_cage_opacity;
};

struct View3D_Runtime	{
	void	*properties_storage;
	void	(*properties_storage_free)();
	int	flag;
	char	_pad1[4];
	void	*local_stats;
};

struct View3D	{
	SpaceLink	*next;
	SpaceLink	*prev;
	ListBase	regionbase;
	char	spacetype;
	char	link_flag;
	char	_pad0[6];
	float	viewquat[4];
	float	dist;
	float	bundle_size;
	char	bundle_drawtype;
	char	drawtype;
	char	_pad3[1];
	char	multiview_eye;
	int	object_type_exclude_viewport;
	int	object_type_exclude_select;
	short	persp;
	short	view;
	Object	*camera;
	Object	*ob_centre;
	rctf	render_border;
	View3D	*localvd;
	char	ob_centre_bone[64];
	short	local_view_uuid;
	char	_pad6[2];
	int	layact;
	short	local_collections_uuid;
	short	_pad7[2];
	short	debug_flag;
	short	ob_centre_cursor;
	short	scenelock;
	short	gp_flag;
	short	flag;
	int	flag2;
	float	lens;
	float	grid;
	float	near;
	float	far;
	float	ofs[3];
	char	_pad[1];
	char	gizmo_flag;
	char	gizmo_show_object;
	char	gizmo_show_armature;
	char	gizmo_show_empty;
	char	gizmo_show_light;
	char	gizmo_show_camera;
	char	gridflag;
	short	gridlines;
	short	gridsubdiv;
	float	vertex_opacity;
	bGPdata	*gpd;
	short	stereo3d_flag;
	char	stereo3d_camera;
	char	_pad4;
	float	stereo3d_convergence_factor;
	float	stereo3d_volume_alpha;
	float	stereo3d_convergence_alpha;
	View3DShading	shading;
	View3DOverlay	overlay;
	ViewerPath	viewer_path;
	View3D_Runtime	runtime;
};

struct ViewerPathElem	{
	ViewerPathElem	*next;
	ViewerPathElem	*prev;
	int	type;
	char	_pad[4];
	char	*ui_name;
};

struct IDViewerPathElem	{
	ViewerPathElem	base;
	ID	*id;
};

struct ModifierViewerPathElem	{
	ViewerPathElem	base;
	int	modifier_uid;
	char	_pad[4];
};

struct GroupNodeViewerPathElem	{
	ViewerPathElem	base;
	int	node_id;
	char	_pad1[4];
};

struct SimulationZoneViewerPathElem	{
	ViewerPathElem	base;
	int	sim_output_node_id;
	char	_pad1[4];
};

struct RepeatZoneViewerPathElem	{
	ViewerPathElem	base;
	int	repeat_output_node_id;
	int	iteration;
};

struct ForeachGeometryElementZoneViewerPathElem	{
	ViewerPathElem	base;
	int	zone_output_node_id;
	int	index;
};

struct ViewerNodeViewerPathElem	{
	ViewerPathElem	base;
	int	node_id;
	char	_pad1[4];
};

struct EvaluateClosureNodeViewerPathElem	{
	ViewerPathElem	base;
	int	evaluate_node_id;
	int	source_output_node_id;
	bNodeTree	*source_node_tree;
};

struct VolumeDisplay	{
	float	density;
	int	wireframe_type;
	int	wireframe_detail;
	int	interpolation_method;
	int	axis_slice_method;
	int	slice_axis;
	float	slice_depth;
	int	_pad[1];
};

struct VolumeRender	{
	int	precision;
	int	space;
	float	step_size;
	float	clipping;
};

struct Volume	{
	ID	id;
	AnimData	*adt;
	char	filepath[1024];
	PackedFile	*packedfile;
	char	is_sequence;
	char	sequence_mode;
	char	_pad1[2];
	int	frame_start;
	int	frame_duration;
	int	frame_offset;
	int	flag;
	int	active_grid;
	Material	**mat;
	short	totcol;
	short	_pad2[3];
	VolumeRender	render;
	VolumeDisplay	display;
	char	velocity_grid[64];
	char	_pad3[3];
	char	velocity_unit;
	float	velocity_scale;
	void	*batch_cache;
	void	*runtime;
};

struct ReportList	{
	ListBase	list;
	int	printlevel;
	int	storelevel;
	int	flag;
	char	_pad[4];
	void	*reporttimer;
	void	*lock;
};

struct XrSessionSettings	{
	View3DShading	shading;
	float	base_scale;
	char	_pad[3];
	char	base_pose_type;
	Object	*base_pose_object;
	float	base_pose_location[3];
	float	base_pose_angle;
	char	draw_flags;
	char	controller_draw_style;
	char	_pad2[2];
	float	clip_start;
	float	clip_end;
	int	flag;
	int	object_type_exclude_viewport;
	int	object_type_exclude_select;
};

struct wmXrData	{
	void	*runtime;
	XrSessionSettings	session_settings;
};

struct wmWindow;
struct wmKeyConfig;
struct wmWindowManager	{
	ID	id;
	wmWindow	*windrawable;
	wmWindow	*winactive;
	ListBase	windows;
	uchar	init_flag;
	char	_pad0[1];
	short	file_saved;
	short	op_undo_depth;
	short	outliner_sync_select_dirty;
	ListBase	operators;
	int	extensions_updates;
	int	extensions_blocked;
	ListBase	jobs;
	ListBase	paintcursors;
	ListBase	drags;
	ListBase	keyconfigs;
	wmKeyConfig	*defaultconf;
	wmKeyConfig	*addonconf;
	wmKeyConfig	*userconf;
	ListBase	timers;
	void	*autosavetimer;
	char	autosave_scheduled;
	char	_pad2[7];
	void	*undo_stack;
	void	*message_bus;
	wmXrData	xr;
	void	*runtime;
};

struct WorkSpaceInstanceHook;
struct wmWindow	{
	wmWindow	*next;
	wmWindow	*prev;
	void	*ghostwin;
	void	*gpuctx;
	wmWindow	*parent;
	Scene	*scene;
	Scene	*new_scene;
	char	view_layer_name[64];
	Scene	*unpinned_scene;
	WorkSpaceInstanceHook	*workspace_hook;
	ScrAreaMap	global_area_map;
	bScreen	*screen;
	int	winid;
	short	posx;
	short	posy;
	short	sizex;
	short	sizey;
	char	windowstate;
	char	active;
	short	cursor;
	short	lastcursor;
	short	modalcursor;
	short	grabcursor;
	short	pie_event_type_lock;
	short	pie_event_type_last;
	char	tag_cursor_refresh;
	char	event_queue_check_click;
	char	event_queue_check_drag;
	char	event_queue_check_drag_handled;
	short	event_queue_consecutive_gesture_type;
	int	event_queue_consecutive_gesture_xy[2];
	void	*event_queue_consecutive_gesture_data;
	void	*eventstate;
	void	*event_last_handled;
	char	addmousemove;
	char	_pad1[7];
	ListBase	handlers;
	ListBase	modalhandlers;
	ListBase	gesture;
	Stereo3dFormat	*stereo3d_format;
	ListBase	drawcalls;
	void	*cursor_keymap_status;
	void	*_pad2;
	uint64_t	eventstate_prev_press_time_ms;
	void	*runtime;
	void	*_pad3;
};

struct wmKeyMapItem	{
	wmKeyMapItem	*next;
	wmKeyMapItem	*prev;
	char	idname[64];
	IDProperty	*properties;
	char	propvalue_str[64];
	short	propvalue;
	short	type;
	int8_t	val;
	int8_t	direction;
	int8_t	shift;
	int8_t	ctrl;
	int8_t	alt;
	int8_t	oskey;
	int8_t	hyper;
	char	_pad0[7];
	short	keymodifier;
	uchar	flag;
	uchar	maptype;
	short	id;
	void	*ptr;
};

struct wmKeyMapDiffItem	{
	wmKeyMapDiffItem	*next;
	wmKeyMapDiffItem	*prev;
	wmKeyMapItem	*remove_item;
	wmKeyMapItem	*add_item;
};

struct wmKeyMap	{
	wmKeyMap	*next;
	wmKeyMap	*prev;
	ListBase	items;
	ListBase	diff_items;
	char	idname[64];
	short	spaceid;
	short	regionid;
	char	owner_id[128];
	short	flag;
	short	kmi_id;
	bool	(*poll)();
	bool	(*poll_modal_item)();
	void	*modal_items;
};

struct wmKeyConfigPref	{
	wmKeyConfigPref	*next;
	wmKeyConfigPref	*prev;
	char	idname[64];
	IDProperty	*prop;
};

struct wmKeyConfig	{
	wmKeyConfig	*next;
	wmKeyConfig	*prev;
	char	idname[64];
	char	basename[64];
	ListBase	keymaps;
	int	actkeymap;
	short	flag;
	char	_pad0[2];
};

struct wmOperator	{
	wmOperator	*next;
	wmOperator	*prev;
	char	idname[64];
	IDProperty	*properties;
	void	*type;
	void	*customdata;
	void	*py_instance;
	void	*ptr;
	ReportList	*reports;
	ListBase	macro;
	wmOperator	*opm;
	void	*layout;
	short	flag;
	char	_pad[6];
};

struct bToolRef	{
	bToolRef	*next;
	bToolRef	*prev;
	char	idname[64];
	char	idname_fallback[64];
	char	idname_pending[64];
	short	tag;
	short	space_type;
	int	mode;
	IDProperty	*properties;
	void	*runtime;
};

struct WorkSpaceLayout	{
	WorkSpaceLayout	*next;
	WorkSpaceLayout	*prev;
	bScreen	*screen;
	char	name[64];
};

struct wmOwnerID	{
	wmOwnerID	*next;
	wmOwnerID	*prev;
	char	name[128];
};

struct WorkSpace	{
	ID	id;
	ListBase	layouts;
	ListBase	hook_layout_relations;
	ListBase	owner_ids;
	ListBase	tools;
	Scene	*pin_scene;
	char	_pad[4];
	int	object_mode;
	int	flags;
	int	order;
	void	*runtime;
	AssetLibraryReference	asset_library_ref;
	ViewerPath	viewer_path;
};

struct WorkSpaceDataRelation	{
	WorkSpaceDataRelation	*next;
	WorkSpaceDataRelation	*prev;
	void	*parent;
	void	*value;
	int	parentid;
	char	_pad_0[4];
};

struct WorkSpaceInstanceHook	{
	WorkSpace	*active;
	WorkSpaceLayout	*act_layout;
	WorkSpace	*temp_workspace_store;
	WorkSpaceLayout	*temp_layout_store;
};

struct World	{
	ID	id;
	AnimData	*adt;
	char	_pad0[4];
	short	texact;
	short	mistype;
	float	horr;
	float	horg;
	float	horb;
	float	exposure;
	float	exp;
	float	range;
	short	mode;
	char	_pad2[6];
	float	misi;
	float	miststa;
	float	mistdist;
	float	misthi;
	float	aodist;
	float	aoenergy;
	short	flag;
	char	_pad3[2];
	int	probe_resolution;
	float	sun_threshold;
	float	sun_angle;
	float	sun_shadow_maximum_resolution;
	float	sun_shadow_jitter_overblur;
	float	sun_shadow_filter_radius;
	char	_pad4[4];
	Ipo	*ipo;
	short	pr_texture;
	short	use_nodes;
	char	_pad[4];
	PreviewImage	*preview;
	bNodeTree	*nodetree;
	LightgroupMembership	*lightgroup;
	void	*_pad1;
	ListBase	gpumaterial;
	uint64_t	last_update;
};

struct XrComponentPath	{
	XrComponentPath	*next;
	XrComponentPath	*prev;
	char	path[192];
};

struct XrActionMapBinding	{
	XrActionMapBinding	*next;
	XrActionMapBinding	*prev;
	char	name[64];
	char	profile[256];
	ListBase	component_paths;
	float	float_threshold;
	short	axis_flag;
	char	_pad[2];
	float	pose_location[3];
	float	pose_rotation[3];
};

struct XrUserPath	{
	XrUserPath	*next;
	XrUserPath	*prev;
	char	path[64];
};

struct XrActionMapItem	{
	XrActionMapItem	*next;
	XrActionMapItem	*prev;
	char	name[64];
	char	type;
	char	_pad[7];
	ListBase	user_paths;
	char	op[64];
	IDProperty	*op_properties;
	void	*op_properties_ptr;
	short	op_flag;
	short	action_flag;
	short	haptic_flag;
	short	pose_flag;
	char	haptic_name[64];
	float	haptic_duration;
	float	haptic_frequency;
	float	haptic_amplitude;
	short	selbinding;
	char	_pad3[2];
	ListBase	bindings;
};

struct XrActionMap	{
	XrActionMap	*next;
	XrActionMap	*prev;
	char	name[64];
	ListBase	items;
	short	selitem;
	char	_pad[6];
};



#ifdef __cplusplus
}	// extern "C"
}	// namespace Blender
#endif // __cplusplus

#endif //BLENDER_H

