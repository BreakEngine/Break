#include "ChainShape.hpp"
#include "EdgeShape.hpp"
#include <new>
#include <memory.h>

using namespace Break;
using namespace Break::Infrastructure;
using namespace Break::Physics;

ChainShape::~ChainShape()
{
	free(m_vertices);
	m_vertices = NULL;
	m_count = 0;
}

void ChainShape::CreateLoop(const glm::vec2* vertices, s32 count)
{
	assert(m_vertices == NULL && m_count == 0);
	assert(count >= 3);
	for (s32 i = 1; i < count; ++i)
	{
		glm::vec2 v1 = vertices[i-1];
		glm::vec2 v2 = vertices[i];

		// If the code crashes here, it means your vertices are too close together.
		assert(MathUtils::DistanceSquared(v1, v2) > linearSlop * linearSlop);
	}

	m_count = count + 1;
	m_vertices = (glm::vec2*)malloc(m_count * sizeof(glm::vec2));
	memcpy(m_vertices, vertices, count * sizeof(glm::vec2));
	m_vertices[count] = m_vertices[0];
	m_prevVertex = m_vertices[m_count - 2];
	m_nextVertex = m_vertices[1];
	m_hasPrevVertex = true;
	m_hasNextVertex = true;
}

void ChainShape::CreateChain(const glm::vec2* vertices, s32 count)
{
	assert(m_vertices == NULL && m_count == 0);
	assert(count >= 2);
	for (s32 i = 1; i < count; ++i)
	{
		glm::vec2 v1 = vertices[i-1];
		glm::vec2 v2 = vertices[i];


		// If the code crashes here, it means your vertices are too close together.
		assert(MathUtils::DistanceSquared(v1, v2) > linearSlop * linearSlop);
	}

	m_count = count;
	m_vertices = (glm::vec2*)malloc(count * sizeof(glm::vec2));
	memcpy(m_vertices, vertices, m_count * sizeof(glm::vec2));

	m_hasPrevVertex = false;
	m_hasNextVertex = false;

	m_prevVertex.x = 0; m_prevVertex.y = 0;
	m_nextVertex.x = 0; m_nextVertex.y = 0;
}

void ChainShape::SetPrevVertex(const glm::vec2& prevVertex)
{
	m_prevVertex = prevVertex;
	m_hasPrevVertex = true;
}

void ChainShape::SetNextVertex(const glm::vec2& nextVertex)
{
	m_nextVertex = nextVertex;
	m_hasNextVertex = true;
}

Shape* ChainShape::Clone(BlockAllocator* allocator) const
{
	void* mem = allocator->Allocate(sizeof(ChainShape));
	ChainShape* clone = new (mem) ChainShape;
	clone->CreateChain(m_vertices, m_count);
	clone->m_prevVertex = m_prevVertex;
	clone->m_nextVertex = m_nextVertex;
	clone->m_hasPrevVertex = m_hasPrevVertex;
	clone->m_hasNextVertex = m_hasNextVertex;
	return clone;
}

s32 ChainShape::GetChildCount() const
{
	// edge count = vertex count - 1
	return m_count - 1;
}

void ChainShape::GetChildEdge(EdgeShape* edge, s32 index) const
{
	assert(0 <= index && index < m_count - 1);
	edge->m_type = Shape::edge;
	edge->m_radius = m_radius;

	edge->m_vertex1 = m_vertices[index + 0];
	edge->m_vertex2 = m_vertices[index + 1];

	if (index > 0)
	{
		edge->m_vertex0 = m_vertices[index - 1];
		edge->m_hasVertex0 = true;
	}
	else
	{
		edge->m_vertex0 = m_prevVertex;
		edge->m_hasVertex0 = m_hasPrevVertex;
	}

	if (index < m_count - 2)
	{
		edge->m_vertex3 = m_vertices[index + 2];
		edge->m_hasVertex3 = true;
	}
	else
	{
		edge->m_vertex3 = m_nextVertex;
		edge->m_hasVertex3 = m_hasNextVertex;
	}
}

bool ChainShape::TestPoint(const Transform2D& xf, const glm::vec2& p) const
{
	NOT_USED(xf);
	NOT_USED(p);
	return false;
}

bool ChainShape::RayCast(RayCastOutput* output, const RayCastInput& input,const Transform2D& xf, s32 childIndex) const
{
	assert(childIndex < m_count);

	EdgeShape edgeShape;

	s32 i1 = childIndex;
	s32 i2 = childIndex + 1;
	if (i2 == m_count)
	{
		i2 = 0;
	}

	edgeShape.m_vertex1 = m_vertices[i1];
	edgeShape.m_vertex2 = m_vertices[i2];

	return edgeShape.RayCast(output, input, xf, 0);
}

void ChainShape::ComputeAABB(AABB* aabb, const Transform2D& xf, s32 childIndex) const
{
	assert(childIndex < m_count);

	s32 i1 = childIndex;
	s32 i2 = childIndex + 1;
	if (i2 == m_count)
	{
		i2 = 0;
	}

	glm::vec2 v1 = Transform2D::Mul(xf, m_vertices[i1]);
	glm::vec2 v2 = Transform2D::Mul(xf, m_vertices[i2]);

	aabb->lowerBound = glm::min(v1, v2);
	aabb->upperBound = glm::max(v1, v2);
}

void ChainShape::ComputeMass(MassData* massData, real32 density) const
{
	NOT_USED(density);

	massData->mass = 0.0f;
	massData->center.x = 0; massData->center.y = 0;
	massData->I = 0.0f;
}
