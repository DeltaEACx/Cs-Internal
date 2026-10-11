#include "game/combat_access.h"

namespace combat_access {
    bool RayHitbox(const Vec3& origin,const Vec3& delta,const Hitbox& hitbox,
                   const systems::bones::data& bone,float& fraction)
    {
        if (!Finite(origin) || !Finite(delta) || !Finite(bone.position) || !Finite(hitbox.mins) ||
            !Finite(hitbox.maxs) || !std::isfinite(hitbox.radius) || hitbox.radius < 0 ||
            !std::isfinite(bone.rotation.x) || !std::isfinite(bone.rotation.y) ||
            !std::isfinite(bone.rotation.z) || !std::isfinite(bone.rotation.w)) return false;
        if (hitbox.radius > .001f)
        {
            const auto a = (hitbox.translation_only ? hitbox.mins : bone.rotation.rotate_vector(hitbox.mins)) + bone.position;
            const auto b = (hitbox.translation_only ? hitbox.maxs : bone.rotation.rotate_vector(hitbox.maxs)) + bone.position;
            return RayCapsule(origin,delta,a,b,hitbox.radius,fraction);
        }
        auto inverse = bone.rotation;
        inverse.x = -inverse.x; inverse.y = -inverse.y; inverse.z = -inverse.z;
        const auto start = hitbox.translation_only ? origin - bone.position : inverse.rotate_vector(origin - bone.position);
        const auto direction = hitbox.translation_only ? delta : inverse.rotate_vector(delta);
        float entry=0,exit=1;
        const auto axis=[&](float p,float d,float low,float high)
        {
            if (std::abs(d)<1.e-8f) return p>=low && p<=high;
            float a=(low-p)/d,b=(high-p)/d;
            if (a>b) std::swap(a,b);
            entry=std::max(entry,a); exit=std::min(exit,b);
            return entry<=exit;
        };
        const bool hit=axis(start.x,direction.x,hitbox.mins.x,hitbox.maxs.x) &&
            axis(start.y,direction.y,hitbox.mins.y,hitbox.maxs.y) &&
            axis(start.z,direction.z,hitbox.mins.z,hitbox.maxs.z);
        if (hit) fraction=entry;
        return hit;
    }
    bool RayCapsule( const math::vector3& ray_origin, const math::vector3& ray_dir, const math::vector3& capsule_a, const math::vector3& capsule_b, float radius, float& out_fraction )
    {
        const auto ab = capsule_b - capsule_a;
        const auto ab_sq = ab.dot( ab );
        const auto oc = ray_origin - capsule_a;
        const auto dir_sq = ray_dir.dot( ray_dir );

        if ( dir_sq < 1e-8f )
        {
            return false;
        }

        auto best_t{ 1.0f };
        auto hit{ false };

        if ( ab_sq > 1e-8f )
        {
            const float m = ab.dot( ray_dir ) / ab_sq;
            const float n = ab.dot( oc ) / ab_sq;

            const auto d_perp = ray_dir - ab * m;
            const auto oc_perp = oc - ab * n;

            const auto a = d_perp.dot( d_perp );
            const auto half_b = d_perp.dot( oc_perp );
            const auto c = oc_perp.dot( oc_perp ) - radius * radius;

            if ( a > 1e-8f )
            {
                const auto disc = half_b * half_b - a * c;
                if ( disc >= 0.0f )
                {
                    const auto sqrt_disc = std::sqrt( disc );

                    for ( int r = 0; r < 2; r++ )
                    {
                        const auto t = ( -half_b + ( r == 0 ? -sqrt_disc : sqrt_disc ) ) / a;
                        if ( t < 0.0f || t >= best_t )
                        {
                            continue;
                        }

                        const auto s = m * t + n;
                        if ( s >= 0.0f && s <= 1.0f )
                        {
                            best_t = t;
                            hit = true;
                            break;
                        }
                    }
                }
            }
        }

        const math::vector3 caps[ ]{ capsule_a, capsule_b };

        for ( int i = 0; i < 2; i++ )
        {
            const auto co = ray_origin - caps[ i ];
            const auto half_b = co.dot( ray_dir );
            const auto c = co.dot( co ) - radius * radius;
            const auto disc = half_b * half_b - dir_sq * c;

            if ( disc < 0.0f )
            {
                continue;
            }

            const auto sqrt_disc = std::sqrt( disc );

            for ( int r = 0; r < 2; r++ )
            {
                const auto t = ( -half_b + ( r == 0 ? -sqrt_disc : sqrt_disc ) ) / dir_sq;
                if ( t < 0.0f || t >= best_t )
                {
                    continue;
                }

                if ( ab_sq > 1e-8f )
                {
                    const auto hit_point = ray_origin + ray_dir * t - caps[ i ];
                    const auto sign = i == 0 ? -1.0f : 1.0f;

                    if ( sign * ab.dot( hit_point ) < 0.0f )
                    {
                        continue;
                    }
                }

                best_t = t;
                hit = true;
                break;
            }
        }

        if ( hit )
        {
            out_fraction = best_t;
        }

        return hit;
    }

}
