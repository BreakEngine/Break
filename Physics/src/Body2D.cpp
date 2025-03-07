#include "Body2D.hpp"
#include "Fixture.hpp"
#include "World2D.hpp"
#include "Contact2D.hpp"
#include "Joint2D.hpp"
#include "Transform2D.hpp"
#include "Sweep.hpp"
#include "Rotation2D.hpp"

using namespace Break;
using namespace Break::Infrastructure;
using namespace Break::Physics;


Body::Body(const BodyDef* bd, World* world)
{
	assert(MathUtils::IsValid(bd->position));
	assert(MathUtils::IsValid(bd->linearVelocity));
	assert(MathUtils::IsVal(bd->angle));
	assert(MathUtils::IsVal(bd->angularVelocity));
	assert(MathUtils::IsVal(bd->angularDamping) && bd->angularDamping >= 0.0f);
	assert(MathUtils::IsVal(bd->linearDamping) && bd->linearDamping >= 0.0f);

	m_flags = 0;

	if (bd->bullet)
	{
		m_flags |= bulletFlag;
	}
	if (bd->fixedRotation)
	{
		m_flags |= fixedRotationFlag;
	}
	if (bd->allowSleep)
	{
		m_flags |= autoSleepFlag;
	}
	if (bd->awake)
	{
		m_flags |= awakeFlag;
	}
	if (bd->active)
	{
		m_flags |= activeFlag;
	}

	m_world = world;

	m_xf.p = bd->position;
	m_xf.q.Set(bd->angle);

	m_sweep.localCenter = glm::vec2(0.0f,0.0f);
	m_sweep.c0 = m_xf.p;
	m_sweep.c = m_xf.p;
	m_sweep.a0 = bd->angle;
	m_sweep.a = bd->angle;
	m_sweep.alpha0 = 0.0f;

	m_jointList = NULL;
	m_contactList = NULL;
	m_prev = NULL;
	m_next = NULL;

	m_linearVelocity = bd->linearVelocity;
	m_angularVelocity = bd->angularVelocity;

	m_linearDamping = bd->linearDamping;
	m_angularDamping = bd->angularDamping;
	m_gravityScale = bd->gravityScale;

	m_force = glm::vec2(0.0f,0.0f);
	m_torque = 0.0f;

	m_sleepTime = 0.0f;

	m_type = bd->type;

	if (m_type == dynamicBody)
	{
		m_mass = 1.0f;
		m_invMass = 1.0f;
	}
	else
	{
		m_mass = 0.0f;
		m_invMass = 0.0f;
	}

	m_I = 0.0f;
	m_invI = 0.0f;

	m_userData = bd->userData;

	m_fixtureList = NULL;
	m_fixtureCount = 0;
}

Body::~Body()
{
	// shapes and joints are destroyed in World::Destroy
}

void Body::SetType(BodyType type)
{
	assert(m_world->IsLocked() == false);
	if (m_world->IsLocked() == true)
	{
		return;
	}

	if (m_type == type)
	{
		return;
	}

	m_type = type;

	ResetMassData();

	if (m_type == staticBody)
	{
		m_linearVelocity = glm::vec2(0.0f,0.0f);
		m_angularVelocity = 0.0f;
		m_sweep.a0 = m_sweep.a;
		m_sweep.c0 = m_sweep.c;
		SynchronizeFixtures();
	}

	SetAwake(true);

	m_force = glm::vec2(0,0);
	m_torque = 0.0f;

	// Delete the attached contacts.
	ContactEdge* ce = m_contactList;
	while (ce)
	{
		ContactEdge* ce0 = ce;
		ce = ce->next;
		m_world->m_contactManager.Destroy(ce0->contact);
	}
	m_contactList = NULL;

	// Touch the proxies so that new contacts will be created (when appropriate)
	BroadPhase* broadPhase = &m_world->m_contactManager.m_broadPhase;
	for (Fixture* f = m_fixtureList; f; f = f->m_next)
	{
		s32 proxyCount = f->m_proxyCount;
		for (s32 i = 0; i < proxyCount; ++i)
		{
			broadPhase->TouchProxy(f->m_proxies[i].proxyId);
		}
	}
}

Fixture* Body::CreateFixture(const FixtureDef* def)
{
	assert(m_world->IsLocked() == false);
	if (m_world->IsLocked() == true)
	{
		return NULL;
	}

	BlockAllocator* allocator = &m_world->m_blockAllocator;

	void* memory = allocator->Allocate(sizeof(Fixture));
	Fixture* fixture = new (memory) Fixture;
	fixture->Create(allocator, this, def);

	if (m_flags & activeFlag)
	{
		BroadPhase* broadPhase = &m_world->m_contactManager.m_broadPhase;
		fixture->CreateProxies(broadPhase, m_xf);
	}

	fixture->m_next = m_fixtureList;
	m_fixtureList = fixture;
	++m_fixtureCount;

	fixture->m_body = this;

	// Adjust mass properties if needed.
	if (fixture->m_density > 0.0f)
	{
		ResetMassData();
	}

	// Let the world know we have a new fixture. This will cause new contacts
	// to be created at the beginning of the next time step.
	m_world->m_flags |= World::newFixture;

	return fixture;
}

Fixture* Body::CreateFixture(const Shape* shape, real32 density)
{
	FixtureDef def;
	def.shape = shape;
	def.density = density;

	return CreateFixture(&def);
}

void Body::DestroyFixture(Fixture* fixture)
{
	assert(m_world->IsLocked() == false);
	if (m_world->IsLocked() == true)
	{
		return;
	}

	assert(fixture->m_body == this);

	// Remove the fixture from this body's singly linked list.
	assert(m_fixtureCount > 0);
	Fixture** node = &m_fixtureList;
	bool found = false;
	while (*node != NULL)
	{
		if (*node == fixture)
		{
			*node = fixture->m_next;
			found = true;
			break;
		}

		node = &(*node)->m_next;
	}

	// You tried to remove a shape that is not attached to this body.
	assert(found);

	// Destroy any contacts associated with the fixture.
	ContactEdge* edge = m_contactList;
	while (edge)
	{
		Contact* c = edge->contact;
		edge = edge->next;

		Fixture* fixtureA = c->GetFixtureA();
		Fixture* fixtureB = c->GetFixtureB();

		if (fixture == fixtureA || fixture == fixtureB)
		{
			// This destroys the contact and removes it from
			// this body's contact list.
			m_world->m_contactManager.Destroy(c);
		}
	}

	BlockAllocator* allocator = &m_world->m_blockAllocator;

	if (m_flags & activeFlag)
	{
		BroadPhase* broadPhase = &m_world->m_contactManager.m_broadPhase;
		fixture->DestroyProxies(broadPhase);
	}

	fixture->Destroy(allocator);
	fixture->m_body = NULL;
	fixture->m_next = NULL;
	fixture->~Fixture();
	allocator->Free(fixture, sizeof(Fixture));

	--m_fixtureCount;

	// Reset the mass data.
	ResetMassData();
}

void Body::ResetMassData()
{
	// Compute mass data from shapes. Each shape has its own density.
	m_mass = 0.0f;
	m_invMass = 0.0f;
	m_I = 0.0f;
	m_invI = 0.0f;
	m_sweep.localCenter = glm::vec2(0.0f,0.0f);

	// Static and kinematic bodies have zero mass.
	if (m_type == staticBody || m_type == kinematicBody)
	{
		m_sweep.c0 = m_xf.p;
		m_sweep.c = m_xf.p;
		m_sweep.a0 = m_sweep.a;
		return;
	}

	assert(m_type == dynamicBody);

	// Accumulate mass over all fixtures.
	glm::vec2 localCenter = glm::vec2(0.0f,0.0f);
	for (Fixture* f = m_fixtureList; f; f = f->m_next)
	{
		if (f->m_density == 0.0f)
		{
			continue;
		}

		MassData massData;
		f->GetMassData(&massData);
		m_mass += massData.mass;
		localCenter += massData.mass * massData.center;
		m_I += massData.I;
	}

	// Compute center of mass.
	if (m_mass > 0.0f)
	{
		m_invMass = 1.0f / m_mass;
		localCenter *= m_invMass;
	}
	else
	{
		// Force all dynamic bodies to have a positive mass.
		m_mass = 1.0f;
		m_invMass = 1.0f;
	}

	if (m_I > 0.0f && (m_flags & fixedRotationFlag) == 0)
	{
		// Center the inertia about the center of mass.
		m_I -= m_mass * glm::dot(localCenter, localCenter);
		assert(m_I > 0.0f);
		m_invI = 1.0f / m_I;

	}
	else
	{
		m_I = 0.0f;
		m_invI = 0.0f;
	}

	// Move center of mass.
	glm::vec2 oldCenter = m_sweep.c;
	m_sweep.localCenter = localCenter;
	m_sweep.c0 = m_sweep.c = Physics::Transform2D::Mul(m_xf, m_sweep.localCenter);

	// Update center of mass velocity.
	m_linearVelocity += MathUtils::Cross2(m_angularVelocity, m_sweep.c - oldCenter);
}

void Body::SetMassData(const MassData* massData)
{
	assert(m_world->IsLocked() == false);
	if (m_world->IsLocked() == true)
	{
		return;
	}

	if (m_type != dynamicBody)
	{
		return;
	}

	m_invMass = 0.0f;
	m_I = 0.0f;
	m_invI = 0.0f;

	m_mass = massData->mass;
	if (m_mass <= 0.0f)
	{
		m_mass = 1.0f;
	}

	m_invMass = 1.0f / m_mass;

	if (massData->I > 0.0f && (m_flags & Body::fixedRotationFlag) == 0)
	{
		m_I = massData->I - m_mass * glm::dot(massData->center, massData->center);
		assert(m_I > 0.0f);
		m_invI = 1.0f / m_I;
	}

	// Move center of mass.
	glm::vec2 oldCenter = m_sweep.c;
	m_sweep.localCenter =  massData->center;
	m_sweep.c0 = m_sweep.c = Transform2D::Mul(m_xf, m_sweep.localCenter);

	// Update center of mass velocity.
	m_linearVelocity += MathUtils::Cross2(m_angularVelocity, m_sweep.c - oldCenter);
}

bool Body::ShouldCollide(const Body* other) const
{
	// At least one body should be dynamic.
	if (m_type != dynamicBody && other->m_type != dynamicBody)
	{
		return false;
	}

	// Does a joint prevent collision?
	for (JointEdge* jn = m_jointList; jn; jn = jn->next)
	{
		if (jn->other == other)
		{
			if (jn->joint->m_collideConnected == false)
			{
				return false;
			}
		}
	}

	return true;
}

void Body::SetTransform2D(const glm::vec2& position, real32 angle)
{
	assert(m_world->IsLocked() == false);
	if (m_world->IsLocked() == true)
	{
		return;
	}

	m_xf.q.Set(angle);
	m_xf.p = position;

	m_sweep.c = Transform2D::Mul(m_xf, m_sweep.localCenter);
	m_sweep.a = angle;

	m_sweep.c0 = m_sweep.c;
	m_sweep.a0 = angle;

	BroadPhase* broadPhase = &m_world->m_contactManager.m_broadPhase;
	for (Fixture* f = m_fixtureList; f; f = f->m_next)
	{
		f->Synchronize(broadPhase, m_xf, m_xf);
	}
}

void Body::SynchronizeFixtures()
{
	Transform2D xf1;
	xf1.q.Set(m_sweep.a0);
	xf1.p = m_sweep.c0 - Rotation2D::Mul(xf1.q, m_sweep.localCenter);

	BroadPhase* broadPhase = &m_world->m_contactManager.m_broadPhase;
	for (Fixture* f = m_fixtureList; f; f = f->m_next)
	{
		f->Synchronize(broadPhase, xf1, m_xf);
	}
}

void Body::SetActive(bool flag)
{
	assert(m_world->IsLocked() == false);

	if (flag == IsActive())
	{
		return;
	}

	if (flag)
	{
		m_flags |= activeFlag;

		// Create all proxies.
		BroadPhase* broadPhase = &m_world->m_contactManager.m_broadPhase;
		for (Fixture* f = m_fixtureList; f; f = f->m_next)
		{
			f->CreateProxies(broadPhase, m_xf);
		}

		// Contacts are created the next time step.
	}
	else
	{
		m_flags &= ~activeFlag;

		// Destroy all proxies.
		BroadPhase* broadPhase = &m_world->m_contactManager.m_broadPhase;
		for (Fixture* f = m_fixtureList; f; f = f->m_next)
		{
			f->DestroyProxies(broadPhase);
		}

		// Destroy the attached contacts.
		ContactEdge* ce = m_contactList;
		while (ce)
		{
			ContactEdge* ce0 = ce;
			ce = ce->next;
			m_world->m_contactManager.Destroy(ce0->contact);
		}
		m_contactList = NULL;
	}
}

void Body::SetFixedRotation(bool flag)
{
	bool status = (m_flags & fixedRotationFlag) == fixedRotationFlag;
	if (status == flag)
	{
		return;
	}

	if (flag)
	{
		m_flags |= fixedRotationFlag;
	}
	else
	{
		m_flags &= ~fixedRotationFlag;
	}

	m_angularVelocity = 0.0f;

	ResetMassData();
}

void Body::Dump()
{
	s32 bodyIndex = m_islandIndex;

	printf("{\n");
	printf("  BodyDef bd;\n");
	printf("  bd.type = BodyType(%d);\n", m_type);
	printf("  bd.position.Set(%.15lef, %.15lef);\n", m_xf.p.x, m_xf.p.y);
	printf("  bd.angle = %.15lef;\n", m_sweep.a);
	printf("  bd.linearVelocity.Set(%.15lef, %.15lef);\n", m_linearVelocity.x, m_linearVelocity.y);
	printf("  bd.angularVelocity = %.15lef;\n", m_angularVelocity);
	printf("  bd.linearDamping = %.15lef;\n", m_linearDamping);
	printf("  bd.angularDamping = %.15lef;\n", m_angularDamping);
	printf("  bd.allowSleep = bool(%d);\n", m_flags & autoSleepFlag);
	printf("  bd.awake = bool(%d);\n", m_flags & awakeFlag);
	printf("  bd.fixedRotation = bool(%d);\n", m_flags & fixedRotationFlag);
	printf("  bd.bullet = bool(%d);\n", m_flags & bulletFlag);
	printf("  bd.active = bool(%d);\n", m_flags & activeFlag);
	printf("  bd.gravityScale = %.15lef;\n", m_gravityScale);
	printf("  bodies[%d] = m_world->CreateBody(&bd);\n", m_islandIndex);
	printf("\n");
	for (Fixture* f = m_fixtureList; f; f = f->m_next)
	{
		printf("  {\n");
		f->Dump(bodyIndex);
		printf("  }\n");
	}
	printf("}\n");
}


inline BodyType Body::GetType() const
{
	return m_type;
}

inline const Transform2D& Body::GetTransform2D() const
{
	return m_xf;
}

inline const glm::vec2& Body::GetPosition() const
{
	return m_xf.p;
}

inline real32 Body::GetAngle() const
{
	return m_sweep.a;
}

inline const glm::vec2& Body::GetWorldCenter() const
{
	return m_sweep.c;
}

inline const glm::vec2& Body::GetLocalCenter() const
{
	return m_sweep.localCenter;
}

inline void Body::SetLinearVelocity(const glm::vec2& v)
{
	if (m_type == staticBody)
	{
		return;
	}

	if (glm::dot(v,v) > 0.0f)
	{
		SetAwake(true);
	}

	m_linearVelocity = v;
}

inline const glm::vec2& Body::GetLinearVelocity() const
{
	return m_linearVelocity;
}

inline void Body::SetAngularVelocity(real32 w)
{
	if (m_type == staticBody)
	{
		return;
	}

	if (w * w > 0.0f)
	{
		SetAwake(true);
	}

	m_angularVelocity = w;
}

inline real32 Body::GetAngularVelocity() const
{
	return m_angularVelocity;
}

inline real32 Body::GetMass() const
{
	return m_mass;
}

inline real32 Body::GetInertia() const
{
	return m_I + m_mass * glm::dot(m_sweep.localCenter, m_sweep.localCenter);
}

inline void Body::GetMassData(MassData* data) const
{
	data->mass = m_mass;
	data->I = m_I + m_mass * glm::dot(m_sweep.localCenter, m_sweep.localCenter);
	data->center = m_sweep.localCenter;
}

inline glm::vec2 Body::GetWorldPoint(const glm::vec2& localPoint) const
{
	return Transform2D::Mul(m_xf, localPoint);
}

inline glm::vec2 Body::GetWorldVector(const glm::vec2& localVector) const
{
	return Rotation2D::Mul(m_xf.q, localVector);
}

inline glm::vec2 Body::GetLocalPoint(const glm::vec2& worldPoint) const
{
	return Transform2D::MulT(m_xf, worldPoint);
}

inline glm::vec2 Body::GetLocalVector(const glm::vec2& worldVector) const
{
	return Rotation2D::MulT(m_xf.q, worldVector);
}

inline glm::vec2 Body::GetLinearVelocityFromWorldPoint(const glm::vec2& worldPoint) const
{
	return m_linearVelocity + MathUtils::Cross2(m_angularVelocity, worldPoint - m_sweep.c);
}

inline glm::vec2 Body::GetLinearVelocityFromLocalPoint(const glm::vec2& localPoint) const
{
	return GetLinearVelocityFromWorldPoint(GetWorldPoint(localPoint));
}

inline real32 Body::GetLinearDamping() const
{
	return m_linearDamping;
}

inline void Body::SetLinearDamping(real32 linearDamping)
{
	m_linearDamping = linearDamping;
}

inline real32 Body::GetAngularDamping() const
{
	return m_angularDamping;
}

inline void Body::SetAngularDamping(real32 angularDamping)
{
	m_angularDamping = angularDamping;
}

inline real32 Body::GetGravityScale() const
{
	return m_gravityScale;
}

inline void Body::SetGravityScale(real32 scale)
{
	m_gravityScale = scale;
}

inline void Body::SetBullet(bool flag)
{
	if (flag)
	{
		m_flags |= bulletFlag;
	}
	else
	{
		m_flags &= ~bulletFlag;
	}
}

inline bool Body::IsBullet() const
{
	return (m_flags & bulletFlag) == bulletFlag;
}

inline void Body::SetAwake(bool flag)
{
	if (flag)
	{
		if ((m_flags & awakeFlag) == 0)
		{
			m_flags |= awakeFlag;
			m_sleepTime = 0.0f;
		}
	}
	else
	{
		m_flags &= ~awakeFlag;
		m_sleepTime = 0.0f;
		m_linearVelocity = glm::vec2(0.0f,0.0f);
		m_angularVelocity = 0.0f;
		m_force = glm::vec2(0.0f,0.0f);
		m_torque = 0.0f;
	}
}

inline bool Body::IsAwake() const
{
	return (m_flags & awakeFlag) == awakeFlag;
}

inline bool Body::IsActive() const
{
	return (m_flags & activeFlag) == activeFlag;
}

inline bool Body::IsFixedRotation() const
{
	return (m_flags & fixedRotationFlag) == fixedRotationFlag;
}

inline void Body::SetSleepingAllowed(bool flag)
{
	if (flag)
	{
		m_flags |= autoSleepFlag;
	}
	else
	{
		m_flags &= ~autoSleepFlag;
		SetAwake(true);
	}
}

inline bool Body::IsSleepingAllowed() const
{
	return (m_flags & autoSleepFlag) == autoSleepFlag;
}

inline Fixture* Body::GetFixtureList()
{
	return m_fixtureList;
}

inline const Fixture* Body::GetFixtureList() const
{
	return m_fixtureList;
}

inline JointEdge* Body::GetJointList()
{
	return m_jointList;
}

inline const JointEdge* Body::GetJointList() const
{
	return m_jointList;
}

inline ContactEdge* Body::GetContactList()
{
	return m_contactList;
}

inline const ContactEdge* Body::GetContactList() const
{
	return m_contactList;
}

inline Body* Body::GetNext()
{
	return m_next;
}

inline const Body* Body::GetNext() const
{
	return m_next;
}

inline void Body::SetUserData(void* data)
{
	m_userData = data;
}

inline void* Body::GetUserData() const
{
	return m_userData;
}

inline void Body::ApplyForce(const glm::vec2& force, const glm::vec2& point, bool wake)
{
	if (m_type != dynamicBody)
	{
		return;
	}

	if (wake && (m_flags & awakeFlag) == 0)
	{
		SetAwake(true);
	}

	// Don't accumulate a force if the body is sleeping.
	if (m_flags & awakeFlag)
	{
		m_force += force;
		m_torque += MathUtils::Cross2(point - m_sweep.c, force);
	}
}

inline void Body::ApplyForceToCenter(const glm::vec2& force, bool wake)
{
	if (m_type != dynamicBody)
	{
		return;
	}

	if (wake && (m_flags & awakeFlag) == 0)
	{
		SetAwake(true);
	}

	// Don't accumulate a force if the body is sleeping
	if (m_flags & awakeFlag)
	{
		m_force += force;
	}
}

inline void Body::ApplyTorque(real32 torque, bool wake)
{
	if (m_type != dynamicBody)
	{
		return;
	}

	if (wake && (m_flags & awakeFlag) == 0)
	{
		SetAwake(true);
	}

	// Don't accumulate a force if the body is sleeping
	if (m_flags & awakeFlag)
	{
		m_torque += torque;
	}
}

inline void Body::ApplyLinearImpulse(const glm::vec2& impulse, const glm::vec2& point, bool wake)
{
	if (m_type != dynamicBody)
	{
		return;
	}

	if (wake && (m_flags & awakeFlag) == 0)
	{
		SetAwake(true);
	}

	// Don't accumulate velocity if the body is sleeping
	if (m_flags & awakeFlag)
	{
		m_linearVelocity += m_invMass * impulse;
		m_angularVelocity += m_invI * MathUtils::Cross2(point - m_sweep.c, impulse);
	}
}

inline void Body::ApplyAngularImpulse(real32 impulse, bool wake)
{
	if (m_type != dynamicBody)
	{
		return;
	}

	if (wake && (m_flags & awakeFlag) == 0)
	{
		SetAwake(true);
	}

	// Don't accumulate velocity if the body is sleeping
	if (m_flags & awakeFlag)
	{
		m_angularVelocity += m_invI * impulse;
	}
}

inline void Body::SynchronizeTransform2D()
{
	m_xf.q.Set(m_sweep.a);
	m_xf.p = m_sweep.c - Rotation2D::Mul(m_xf.q, m_sweep.localCenter);
}

inline void Body::Advance(real32 alpha)
{
	// Advance to the new safe time. This doesn't sync the broad-phase.
	m_sweep.Advance(alpha);
	m_sweep.c = m_sweep.c0;
	m_sweep.a = m_sweep.a0;
	m_xf.q.Set(m_sweep.a);
	m_xf.p = m_sweep.c - Rotation2D::Mul(m_xf.q, m_sweep.localCenter);
}

inline World* Body::GetWorld()
{
	return m_world;
}

inline const World* Body::GetWorld() const
{
	return m_world;
}